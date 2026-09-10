# syntax=docker/dockerfile:1

FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/zvuk-server .

FROM scratch
COPY --from=build /out/zvuk-server /zvuk-server
EXPOSE 8080
ENTRYPOINT ["/zvuk-server"]
