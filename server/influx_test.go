package main

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeInflux struct {
	mu     sync.Mutex
	status []int // risposte in ordine, poi 204
	lines  []string
	query  string
	auth   string
}

func (f *fakeInflux) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	body, _ := io.ReadAll(r.Body)
	f.query, f.auth = r.URL.RequestURI(), r.Header.Get("Authorization")
	status := http.StatusNoContent
	if len(f.status) > 0 {
		status = f.status[0]
		f.status = f.status[1:]
	}
	if status < 300 {
		f.lines = append(f.lines, string(body))
	}
	w.WriteHeader(status)
}

func (f *fakeInflux) received() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.lines...)
}

func startWriter(t *testing.T, f *fakeInflux) (*InfluxWriter, context.CancelFunc, chan struct{}) {
	t.Helper()
	return startWriterWithRetry(t, f, time.Millisecond, 5*time.Millisecond)
}

func startWriterWithRetry(t *testing.T, f *fakeInflux, retryMin, retryMax time.Duration) (*InfluxWriter, context.CancelFunc, chan struct{}) {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	w := NewInfluxWriter(srv.URL+"/", "segreto", "org", "bucket")
	w.retryMin, w.retryMax = retryMin, retryMax
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	done := make(chan struct{})
	go func() {
		w.Run(ctx)
		close(done)
	}()
	return w, cancel, done
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatal("condizione non raggiunta in tempo")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestWriterSendsRequest(t *testing.T) {
	f := &fakeInflux{}
	w, _, _ := startWriter(t, f)
	w.Enqueue("riga")
	waitFor(t, func() bool { return len(f.received()) == 1 })
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.auth != "Token segreto" {
		t.Errorf("auth: %q", f.auth)
	}
	for _, part := range []string{"/api/v2/write?", "org=org", "bucket=bucket", "precision=s"} {
		if !strings.Contains(f.query, part) {
			t.Errorf("query %q senza %q", f.query, part)
		}
	}
}

// Con InfluxDB fermo o il token sbagliato i dati non vanno persi, e restano in ordine
func TestWriterRetriesKeepingOrder(t *testing.T) {
	f := &fakeInflux{status: []int{500, 503, 401, 404}}
	w, _, _ := startWriter(t, f)
	w.Enqueue("a")
	w.Enqueue("b")
	w.Enqueue("c")
	waitFor(t, func() bool { return len(f.received()) == 3 })
	if got := strings.Join(f.received(), ""); got != "abc" {
		t.Fatalf("ordine errato: %s", got)
	}
}

// Un punto rifiutato come malformato non deve bloccare per sempre quelli successivi
func TestWriterDropsRejectedPoint(t *testing.T) {
	f := &fakeInflux{status: []int{400}}
	w, _, _ := startWriter(t, f)
	w.Enqueue("malformato")
	w.Enqueue("buono")
	waitFor(t, func() bool { return len(f.received()) == 1 })
	if f.received()[0] != "buono" {
		t.Fatalf("ricevuto %v", f.received())
	}
}

func TestWriterQueueDropsOldest(t *testing.T) {
	w := NewInfluxWriter("http://127.0.0.1:1", "", "o", "b")
	for i := 0; i < queueSize+5; i++ {
		w.Enqueue(string(rune('A' + i%26)))
	}
	if w.pending() != queueSize {
		t.Fatalf("coda: %d", w.pending())
	}
	if first, _ := w.next(); first != string(rune('A'+5%26)) {
		t.Fatalf("primo in coda: %q", first)
	}
}

// All'arresto si scrive quello che è rimasto in coda
func TestWriterFlushesOnShutdown(t *testing.T) {
	f := &fakeInflux{status: []int{500}}
	w, cancel, done := startWriterWithRetry(t, f, time.Hour, time.Hour) // non riprova da solo
	w.Enqueue("a")
	time.Sleep(50 * time.Millisecond)
	cancel()
	<-done
	if got := f.received(); len(got) != 1 || got[0] != "a" {
		t.Fatalf("ricevuto %v", got)
	}
}
