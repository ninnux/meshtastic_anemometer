package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

const (
	// Circa una settimana di dati per una stazione con un report ogni 5 minuti.
	queueSize       = 2048
	retryMinDelay   = 5 * time.Second
	retryMaxDelay   = time.Minute
	writeTimeout    = 10 * time.Second
	shutdownTimeout = 5 * time.Second
)

// InfluxWriter scrive su InfluxDB v2 in background. Se InfluxDB non è raggiungibile
// conserva i punti in memoria, con il loro orario, e riprova: ogni report vale 5 minuti
// di dati. Si perdono solo i punti più vecchi a coda piena e quelli ancora in coda se
// il server viene fermato con InfluxDB irraggiungibile.
type InfluxWriter struct {
	writeURL string
	token    string
	client   *http.Client

	mu    sync.Mutex
	queue []string
	wake  chan struct{}

	retryMin time.Duration
	retryMax time.Duration
}

func NewInfluxWriter(baseURL, token, org, bucket string) *InfluxWriter {
	query := url.Values{"org": {org}, "bucket": {bucket}, "precision": {"s"}}
	return &InfluxWriter{
		writeURL: strings.TrimRight(baseURL, "/") + "/api/v2/write?" + query.Encode(),
		token:    token,
		client:   &http.Client{Timeout: writeTimeout},
		wake:     make(chan struct{}, 1),
		retryMin: retryMinDelay,
		retryMax: retryMaxDelay,
	}
}

// Enqueue non blocca mai, così una InfluxDB lenta non ferma la ricezione CoAP.
func (w *InfluxWriter) Enqueue(line string) {
	w.mu.Lock()
	if len(w.queue) >= queueSize {
		w.queue = w.queue[1:]
		log.Print("coda InfluxDB piena: scartato il punto più vecchio")
	}
	w.queue = append(w.queue, line)
	w.mu.Unlock()

	select {
	case w.wake <- struct{}{}:
	default:
	}
}

func (w *InfluxWriter) next() (string, bool) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.queue) == 0 {
		return "", false
	}
	return w.queue[0], true
}

func (w *InfluxWriter) pop() {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.queue) > 0 {
		w.queue = w.queue[1:]
	}
}

func (w *InfluxWriter) pending() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return len(w.queue)
}

// Run scrive finché ctx non viene annullato, poi prova a svuotare la coda.
func (w *InfluxWriter) Run(ctx context.Context) {
	delay := w.retryMin
	for {
		line, ok := w.next()
		if !ok {
			select {
			case <-w.wake:
				continue
			case <-ctx.Done():
				return
			}
		}

		err := w.write(ctx, line)
		if err == nil || isPermanent(err) {
			if err != nil {
				log.Printf("punto scartato da InfluxDB: %v", err)
			}
			w.pop()
			delay = w.retryMin
			continue
		}

		if ctx.Err() != nil {
			w.flush()
			return
		}
		log.Printf("scrittura su InfluxDB fallita, nuovo tentativo tra %s: %v", delay, err)
		select {
		case <-time.After(delay):
		case <-ctx.Done():
			w.flush()
			return
		}
		delay = min(delay*2, w.retryMax)
	}
}

func (w *InfluxWriter) flush() {
	ctx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer cancel()
	for {
		line, ok := w.next()
		if !ok {
			return
		}
		err := w.write(ctx, line)
		if err != nil && !isPermanent(err) {
			log.Printf("arresto: %d punti non scritti su InfluxDB: %v", w.pending(), err)
			return
		}
		w.pop()
	}
}

// permanentError è un rifiuto che non cambia riprovando, ad esempio un punto malformato.
type permanentError struct{ message string }

func (e permanentError) Error() string { return e.message }

func isPermanent(err error) bool {
	_, ok := err.(permanentError)
	return ok
}

func (w *InfluxWriter) write(ctx context.Context, line string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, w.writeURL, strings.NewReader(line))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Token "+w.token)
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")

	resp, err := w.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))

	switch {
	case resp.StatusCode < 300:
		return nil
	case resp.StatusCode == http.StatusBadRequest || resp.StatusCode == http.StatusRequestEntityTooLarge || resp.StatusCode == http.StatusUnprocessableEntity:
		return permanentError{fmt.Sprintf("HTTP %d: %s", resp.StatusCode, body)}
	default:
		// Compresi 401 e 404: un token o un bucket sbagliati si correggono senza perdere dati
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, body)
	}
}
