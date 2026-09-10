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

type Server struct {
	mqtt      mqtt.Client
	mu        sync.RWMutex
	devices   map[string]*Device
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
		dataDir:   dataDir,
		playBase:  playBase,
		mqttReady: make(chan struct{}),
	}

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
	mux.HandleFunc("GET /audio/{name}", s.handleAudio)
	mux.Handle("GET /", http.FileServerFS(staticFS))

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

func (s *Server) handlePlay(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !deviceIDRe.MatchString(id) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "некорректный id устройства"})
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, maxUploadBytes)
	file, _, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "нужен multipart с полем file (макс 15 МБ)"})
		return
	}
	defer file.Close()

	name := randHex(8) + ".mp3"
	dst := filepath.Join(s.dataDir, name)
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "не удалось сохранить файл"})
		return
	}
	if _, err := io.Copy(out, file); err != nil {
		out.Close()
		os.Remove(dst)
		writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{"error": "файл слишком большой"})
		return
	}
	out.Close()

	playURL := s.playBase + "/" + name
	if err := s.publishCmd(id, "play "+playURL); err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "mqtt: " + err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "url": playURL, "name": name})
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
	case strings.HasPrefix(cmd, "play "):
		raw := strings.TrimSpace(strings.TrimPrefix(cmd, "play "))
		u, err := url.Parse(raw)
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			return errors.New("play: нужен http(s) URL")
		}
		if strings.ContainsAny(raw, "\r\n") {
			return errors.New("play: недопустимые символы в URL")
		}
		return nil
	default:
		return errors.New("неизвестная команда; допустимы: beep, stop, vol N, play URL")
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
