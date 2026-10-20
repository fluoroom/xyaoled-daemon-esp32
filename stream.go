package main

import (
	"log"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Streamer shows frames received over WebSocket in the live program (3),
// at most fps per second. Only the newest frame is kept: frames arriving
// faster than the panel can take them are dropped rather than queued, so
// the picture never lags behind. A client can override the rate (1-30 fps).
// The playlist resumes, and the configured rate comes back, resumeDelay
// after the last client disconnects; a client reconnecting within that
// window (a dropped socket, a camera hiccup) keeps the live view.
type Streamer struct {
	display     *Display
	defaultRate time.Duration
	resumeDelay time.Duration

	mu       sync.Mutex
	interval time.Duration
	latest   []byte
	clients  int
	resume   bool
	wake     chan struct{}
	pending  *time.Timer // resumeDelay countdown after the last disconnect
}

const maxStreamFPS = 30

func fpsInterval(fps int) time.Duration {
	return time.Second / time.Duration(max(1, min(maxStreamFPS, fps)))
}

// NewStreamer starts the goroutine that sends frames to display.
func NewStreamer(display *Display, fps int, resumeDelay time.Duration) *Streamer {
	s := &Streamer{
		display:     display,
		defaultRate: fpsInterval(fps),
		resumeDelay: resumeDelay,
		interval:    fpsInterval(fps),
		wake:        make(chan struct{}, 1),
	}
	go s.run()
	return s
}

func (s *Streamer) signal() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

func (s *Streamer) push(frame []byte) {
	s.mu.Lock()
	s.latest = frame
	s.mu.Unlock()
	s.signal()
}

// setFPS overrides the send rate for the current stream.
func (s *Streamer) setFPS(fps int) {
	s.mu.Lock()
	s.interval = fpsInterval(fps)
	s.mu.Unlock()
	log.Printf("Stream rate set to %d fps", max(1, min(maxStreamFPS, fps)))
}

func (s *Streamer) connect() {
	s.mu.Lock()
	s.clients++
	s.resume = false
	if s.pending != nil {
		s.pending.Stop()
		s.pending = nil
	}
	s.mu.Unlock()
	s.display.streaming.Store(true)
}

func (s *Streamer) disconnect() {
	s.mu.Lock()
	s.clients--
	if s.clients == 0 {
		// The panel keeps showing the last frame meanwhile.
		s.latest = nil
		s.pending = time.AfterFunc(s.resumeDelay, s.expire)
	}
	s.mu.Unlock()
}

// expire resumes the playlist once resumeDelay has passed with no client.
func (s *Streamer) expire() {
	s.mu.Lock()
	if s.clients == 0 { // a client that reconnected just as the timer fired wins
		s.pending = nil
		s.resume = true
		s.interval = s.defaultRate
	}
	s.mu.Unlock()
	s.signal()
}

// run sends frames and resumes the playlist from one goroutine, so a frame
// still being sent can't land after the resume and re-enter the live view.
// Playlist requests stay blocked from the first connect until that resume.
func (s *Streamer) run() {
	for range s.wake {
		s.mu.Lock()
		frame, resume, interval := s.latest, s.resume, s.interval
		s.latest, s.resume = nil, false
		s.mu.Unlock()

		if resume {
			s.display.streaming.Store(false)
			if err := s.display.resumePlaylist(); err != nil {
				log.Printf("Stream ended, resume playlist failed: %v", err)
			} else {
				log.Println("Stream ended, playlist resumed")
			}
			continue
		}
		if frame == nil {
			continue
		}

		start := time.Now()
		if err := s.display.ShowLive(frame); err != nil {
			log.Printf("Stream frame failed: %v", err)
		}
		time.Sleep(interval - time.Since(start))
		// A frame that arrived while sending is still waiting; pick it up.
		s.signal()
	}
}

var upgrader = websocket.Upgrader{ReadBufferSize: 64 << 10, WriteBufferSize: 1024}

// HandleStream handles the /stream WebSocket: every binary message is an
// image (PNG, JPEG or GIF) shown in the live program. ?fps=N or a text
// message "fps=N" overrides STREAM_FPS (1-30). Closing the socket goes back
// to the playlist.
func (s *Streamer) HandleStream(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return // Upgrade already wrote the error response
	}
	defer conn.Close()
	conn.SetReadLimit(4 << 20)

	s.connect()
	defer s.disconnect()
	if fps, err := strconv.Atoi(r.URL.Query().Get("fps")); err == nil {
		s.setFPS(fps)
	}
	log.Printf("Stream client connected (%s)", r.RemoteAddr)

	for {
		kind, data, err := conn.ReadMessage()
		if err != nil {
			log.Printf("Stream client disconnected (%s)", r.RemoteAddr)
			return
		}
		switch {
		case kind == websocket.BinaryMessage && len(data) > 0:
			s.push(data)
		case kind == websocket.TextMessage && strings.HasPrefix(string(data), "fps="):
			if fps, err := strconv.Atoi(strings.TrimPrefix(string(data), "fps=")); err == nil {
				s.setFPS(fps)
			}
		}
	}
}
