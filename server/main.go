package main

import (
	"context"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

//go:embed static
var staticFS embed.FS

func staticFiles() http.Handler {
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic(err)
	}
	return http.FileServerFS(sub.(fs.ReadDirFS))
}

const (
	maxUploadBytes = 15 << 20 // 15 МБ
	stateTopic     = "zvuk/+/state"
)

var (
	// zvuk- + 6 hex у прошивки; допускаем чуть шире для тестов (zvuk-test),
	// но без +, #, / — иначе инъекция в MQTT-топик.
	deviceIDRe = regexp.MustCompile(`^zvuk-[a-z0-9-]{1,32}$`)
	// имена аудиофайлов генерируем сами: hex + .mp3
	audioNameRe = regexp.MustCompile(`^[0-9a-f]{16}\.mp3$`)
)

type Device struct {
	ID       string    `json:"id"`
	Online   bool      `json:"online"`
	IP       string    `json:"ip"`
	Playing  bool      `json:"playing"`
	Volume   int       `json:"volume"`
	LastSeen time.Time `json:"lastSeen"`
}

type statePayload struct {
	Online  *bool  `json:"online"`
	ID      string `json:"id"`
	IP      string `json:"ip"`
	Playing *bool  `json:"playing"`
	Volume  *int   `json:"volume"`
}

type Track struct {
	Name       string    `json:"name"` // hex-имя файла, <name>.mp3
	Title      string    `json:"title"`
	Size       int64     `json:"size"`
	UploadedAt time.Time `json:"uploadedAt"`
}

type Server struct {
	mqtt      mqtt.Client
	mu        sync.RWMutex
	devices   map[string]*Device
	tmu       sync.Mutex // галерея: tracks + tracks.json
	tracks    map[string]*Track
	dataDir   string
	playBase  string // например http://zvuk.devdima.ru/audio
	mqttReady chan struct{}
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	mqttURL := getenv("MQTT_URL", "tcp://mosquitto.iot-sad-zvuk.svc.cluster.local:1883")
	mqttUser := getenv("MQTT_USER", "backend")
	mqttPass := os.Getenv("MQTT_PASS")
	if mqttPass == "" {
		log.Fatal("MQTT_PASS не задан")
	}
	dataDir := getenv("DATA_DIR", "/data")
	playBase := strings.TrimSuffix(getenv("PLAY_BASE_URL", "http://zvuk.devdima.ru/audio"), "/")
	listen := getenv("LISTEN_ADDR", ":8080")

	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		log.Fatalf("data dir: %v", err)
	}

	s := &Server{
		devices:   make(map[string]*Device),
		tracks:    make(map[string]*Track),
		dataDir:   dataDir,
		playBase:  playBase,
		mqttReady: make(chan struct{}),
	}
	s.loadTracks()

	opts := mqtt.NewClientOptions().
		AddBroker(mqttURL).
		SetClientID("zvuk-server-" + randHex(4)).
		SetUsername(mqttUser).
		SetPassword(mqttPass).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(3 * time.Second).
		SetOnConnectHandler(func(c mqtt.Client) {
			log.Printf("mqtt: подключено к %s", mqttURL)
			if t := c.Subscribe(stateTopic, 1, s.onState); t.WaitTimeout(10*time.Second) && t.Error() != nil {
				log.Printf("mqtt: подписка на %s: %v", stateTopic, t.Error())
				return
			}
			log.Printf("mqtt: подписка на %s активна", stateTopic)
			select {
			case <-s.mqttReady:
			default:
				close(s.mqttReady)
			}
		}).
		SetConnectionLostHandler(func(_ mqtt.Client, err error) {
			log.Printf("mqtt: соединение потеряно: %v", err)
		})

	s.mqtt = mqtt.NewClient(opts)
	s.mqtt.Connect() // ConnectRetry=true — будет переподключаться сам

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/devices", s.handleListDevices)
	mux.HandleFunc("POST /api/devices/{id}/cmd", s.handleCmd)
	mux.HandleFunc("POST /api/devices/{id}/play", s.handlePlay)
	mux.HandleFunc("GET /api/audio", s.handleListAudio)
	mux.HandleFunc("POST /api/audio", s.handleUploadAudio)
	mux.HandleFunc("POST /api/audio/{name}/play", s.handlePlayAudio)
	mux.HandleFunc("PATCH /api/audio/{name}", s.handleRenameAudio)
	mux.HandleFunc("DELETE /api/audio/{name}", s.handleDeleteAudio)
	mux.HandleFunc("GET /audio/{name}", s.handleAudio)
	mux.Handle("GET /", staticFiles())

	httpSrv := &http.Server{
		Addr:              listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		log.Printf("http: слушаю %s", listen)
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("http: %v", err)
		}
	}()

	<-ctx.Done()
	log.Printf("завершение работы...")
	shCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := httpSrv.Shutdown(shCtx); err != nil {
		log.Printf("http shutdown: %v", err)
	}
	s.mqtt.Disconnect(1000)
	log.Printf("остановлено")
}

// --- MQTT ---

func (s *Server) onState(_ mqtt.Client, msg mqtt.Message) {
	// топик zvuk/{id}/state
	parts := strings.Split(msg.Topic(), "/")
	if len(parts) != 3 {
		return
	}
	devID := parts[1]
	if !deviceIDRe.MatchString(devID) {
		return
	}

	var p statePayload
	if err := json.Unmarshal(msg.Payload(), &p); err != nil {
		log.Printf("mqtt: невалидный state JSON от %s: %v", devID, err)
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	d, ok := s.devices[devID]
	if !ok {
		d = &Device{ID: devID}
		s.devices[devID] = d
	}
	if p.Online != nil {
		d.Online = *p.Online
	}
	if p.IP != "" {
		d.IP = p.IP
	}
	if p.Playing != nil {
		d.Playing = *p.Playing
	}
	if p.Volume != nil {
		d.Volume = *p.Volume
	}
	d.LastSeen = time.Now()
	log.Printf("mqtt: state %s online=%v playing=%v vol=%d", devID, d.Online, d.Playing, d.Volume)
}

func (s *Server) publishCmd(id, cmd string) error {
	select {
	case <-s.mqttReady:
	case <-time.After(5 * time.Second):
		return errors.New("mqtt не подключён")
	}
	topic := "zvuk/" + id + "/cmd"
	t := s.mqtt.Publish(topic, 1, false, cmd)
	if t.WaitTimeout(5*time.Second) && t.Error() != nil {
		return t.Error()
	}
	log.Printf("mqtt: -> %s: %q", topic, cmd)
	return nil
}

// --- HTTP ---

func (s *Server) handleListDevices(w http.ResponseWriter, _ *http.Request) {
	s.mu.RLock()
	list := make([]*Device, 0, len(s.devices))
	for _, d := range s.devices {
		cp := *d
		list = append(list, &cp)
	}
	s.mu.RUnlock()
	writeJSON(w, http.StatusOK, list)
}

func (s *Server) handleCmd(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !deviceIDRe.MatchString(id) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "некорректный id устройства"})
		return
	}
	var body struct {
		Cmd string `json:"cmd"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "невалидный JSON"})
		return
	}
	if err := validateCmd(body.Cmd); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	if err := s.publishCmd(id, body.Cmd); err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "mqtt: " + err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// storeUpload сохраняет multipart-файл (поле file) как <hex>.mp3 в dataDir
// и регистрирует трек в галерее. При ошибке ответ уже отправлен, ok=false.
func (s *Server) storeUpload(w http.ResponseWriter, r *http.Request) (t *Track, ok bool) {
	r.Body = http.MaxBytesReader(w, r.Body, maxUploadBytes)
	file, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "нужен multipart с полем file (макс 15 МБ)"})
		return nil, false
	}
	defer file.Close()

	name := randHex(8) + ".mp3"
	dst := filepath.Join(s.dataDir, name)
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "не удалось сохранить файл"})
		return nil, false
	}
	size, err := io.Copy(out, file)
	out.Close()
	if err != nil {
		os.Remove(dst)
		writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{"error": "файл слишком большой"})
		return nil, false
	}

	title := header.Filename
	if title == "" {
		title = name
	}
	return s.addTrack(name, title, size), true
}

func (s *Server) handlePlay(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !deviceIDRe.MatchString(id) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "некорректный id устройства"})
		return
	}

	t, ok := s.storeUpload(w, r)
	if !ok {
		return
	}

	playURL := s.playBase + "/" + t.Name
	if err := s.publishCmd(id, "play "+playURL); err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "mqtt: " + err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "url": playURL, "name": t.Name, "title": t.Title})
}

func (s *Server) handleAudio(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !audioNameRe.MatchString(name) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	f, err := os.Open(filepath.Join(s.dataDir, name))
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	w.Header().Set("Content-Type", "audio/mpeg")
	w.Header().Set("Content-Length", strconv.FormatInt(st.Size(), 10))
	io.Copy(w, f)
}

// --- галерея звуков ---

// addTrack регистрирует трек и сохраняет tracks.json.
func (s *Server) addTrack(name, title string, size int64) *Track {
	t := &Track{Name: name, Title: title, Size: size, UploadedAt: time.Now()}
	s.tmu.Lock()
	s.tracks[name] = t
	err := s.saveTracksLocked()
	s.tmu.Unlock()
	if err != nil {
		log.Printf("галерея: не удалось сохранить tracks.json: %v", err)
	}
	return t
}

func (s *Server) tracksPath() string { return filepath.Join(s.dataDir, "tracks.json") }

// saveTracksLocked пишет tracks.json атомарно (tmp + rename). Вызывать под s.tmu.
func (s *Server) saveTracksLocked() error {
	tmp := s.tracksPath() + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	if err := json.NewEncoder(f).Encode(s.tracks); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(tmp, s.tracksPath())
}

// loadTracks читает tracks.json и подхватывает mp3-файлы без метаданных
// (например, залитые до появления галереи).
func (s *Server) loadTracks() {
	if data, err := os.ReadFile(s.tracksPath()); err == nil {
		if err := json.Unmarshal(data, &s.tracks); err != nil {
			log.Printf("галерея: битый tracks.json, игнорирую: %v", err)
		}
	}
	entries, err := os.ReadDir(s.dataDir)
	if err != nil {
		log.Printf("галерея: не удалось прочитать %s: %v", s.dataDir, err)
		return
	}
	changed := false
	for _, e := range entries {
		if e.IsDir() || !audioNameRe.MatchString(e.Name()) {
			continue
		}
		if _, ok := s.tracks[e.Name()]; ok {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		s.tracks[e.Name()] = &Track{Name: e.Name(), Title: e.Name(),
			Size: info.Size(), UploadedAt: info.ModTime()}
		changed = true
	}
	if changed {
		if err := s.saveTracksLocked(); err != nil {
			log.Printf("галерея: не удалось сохранить tracks.json: %v", err)
		}
	}
	log.Printf("галерея: %d треков в %s", len(s.tracks), s.dataDir)
}

func (s *Server) handleListAudio(w http.ResponseWriter, _ *http.Request) {
	s.tmu.Lock()
	list := make([]*Track, 0, len(s.tracks))
	for _, t := range s.tracks {
		cp := *t
		list = append(list, &cp)
	}
	s.tmu.Unlock()
	sort.Slice(list, func(i, j int) bool { return list[i].UploadedAt.After(list[j].UploadedAt) })
	writeJSON(w, http.StatusOK, list)
}

// POST /api/audio — загрузка трека в галерею без воспроизведения.
func (s *Server) handleUploadAudio(w http.ResponseWriter, r *http.Request) {
	t, ok := s.storeUpload(w, r)
	if !ok {
		return
	}
	writeJSON(w, http.StatusOK, t)
}

// POST /api/audio/{name}/play — проиграть трек из галереи на устройстве.
// Тело: {"device":"zvuk-xxx","loop":false}.
func (s *Server) handlePlayAudio(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !audioNameRe.MatchString(name) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	var body struct {
		Device string `json:"device"`
		Loop   bool   `json:"loop"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "невалидный JSON"})
		return
	}
	if !deviceIDRe.MatchString(body.Device) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "некорректный id устройства"})
		return
	}
	if _, err := os.Stat(filepath.Join(s.dataDir, name)); err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "трек не найден"})
		return
	}
	cmd := "play "
	if body.Loop {
		cmd = "loop "
	}
	playURL := s.playBase + "/" + name
	if err := s.publishCmd(body.Device, cmd+playURL); err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "mqtt: " + err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "url": playURL})
}

// PATCH /api/audio/{name} — переименование трека: {"title":"..."}.
func (s *Server) handleRenameAudio(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !audioNameRe.MatchString(name) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	var body struct {
		Title string `json:"title"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "невалидный JSON"})
		return
	}
	title := strings.TrimSpace(body.Title)
	if title == "" || len(title) > 200 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "title: 1-200 символов"})
		return
	}
	s.tmu.Lock()
	t, ok := s.tracks[name]
	if ok {
		t.Title = title
		s.saveTracksLocked()
	}
	s.tmu.Unlock()
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "трек не найден"})
		return
	}
	writeJSON(w, http.StatusOK, t)
}

// DELETE /api/audio/{name} — удалить трек и файл.
func (s *Server) handleDeleteAudio(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !audioNameRe.MatchString(name) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}
	s.tmu.Lock()
	_, ok := s.tracks[name]
	if ok {
		delete(s.tracks, name)
		s.saveTracksLocked()
	}
	s.tmu.Unlock()
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "трек не найден"})
		return
	}
	if err := os.Remove(filepath.Join(s.dataDir, name)); err != nil {
		log.Printf("галерея: не удалось удалить %s: %v", name, err)
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// --- валидация команд ---

func validateCmd(cmd string) error {
	cmd = strings.TrimSpace(cmd)
	switch {
	case cmd == "beep" || cmd == "stop":
		return nil
	case strings.HasPrefix(cmd, "vol "):
		n, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(cmd, "vol ")))
		if err != nil || n < 0 || n > 255 {
			return errors.New("vol: нужно целое 0-255")
		}
		return nil
	case strings.HasPrefix(cmd, "play "), strings.HasPrefix(cmd, "loop "):
		verb := cmd[:strings.IndexByte(cmd, ' ')]
		raw := strings.TrimSpace(cmd[len(verb)+1:])
		u, err := url.Parse(raw)
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			return errors.New(verb + ": нужен http(s) URL")
		}
		if strings.ContainsAny(raw, "\r\n") {
			return errors.New(verb + ": недопустимые символы в URL")
		}
		return nil
	default:
		return errors.New("неизвестная команда; допустимы: beep, stop, vol N, play URL, loop URL")
	}
}

// --- утилиты ---

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func randHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(fmt.Sprintf("rand: %v", err))
	}
	return hex.EncodeToString(b)
}
