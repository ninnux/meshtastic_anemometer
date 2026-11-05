package main

import (
	"bytes"
	"context"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/message/pool"
	coapnet "github.com/plgd-dev/go-coap/v3/net"
	"github.com/plgd-dev/go-coap/v3/options"
	"github.com/plgd-dev/go-coap/v3/udp"
	udpclient "github.com/plgd-dev/go-coap/v3/udp/client"
	"github.com/plgd-dev/go-coap/v3/udp/coder"
)

type collected struct {
	mu    sync.Mutex
	lines []string
}

func (c *collected) store(line string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.lines = append(c.lines, line)
}

func (c *collected) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.lines)
}

func startServer(t *testing.T, c *collected) string {
	t.Helper()
	router, err := newRouter(c.store)
	if err != nil {
		t.Fatal(err)
	}
	l, err := coapnet.NewListenUDP("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := udp.NewServer(options.WithMux(router))
	done := make(chan struct{})
	go func() {
		_ = s.Serve(l)
		close(done)
	}()
	t.Cleanup(func() {
		s.Stop()
		<-done
		l.Close()
	})
	return l.LocalAddr().String()
}

func dial(t *testing.T, addr string) *udpclient.Conn {
	t.Helper()
	conn, err := udp.Dial(addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func postCON(t *testing.T, conn *udpclient.Conn, path, payload string) codes.Code {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := conn.Post(ctx, path, message.AppJSON, strings.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	return resp.Code()
}

const validPayload = `{"id":"first_anemometer","mean":10.8,"max":15.1,"dir":225}`

func TestConfirmableValid(t *testing.T) {
	c := &collected{}
	conn := dial(t, startServer(t, c))
	if code := postCON(t, conn, "/wind", validPayload); code != codes.Changed {
		t.Fatalf("codice %v", code)
	}
	if c.count() != 1 || !strings.HasPrefix(c.lines[0], "meteo,sensor_name=first_anemometer wind_mean=10.8,wind_max=15.1,direction_deg=225i ") {
		t.Fatalf("righe: %v", c.lines)
	}
}

func TestConfirmableInvalid(t *testing.T) {
	c := &collected{}
	conn := dial(t, startServer(t, c))
	if code := postCON(t, conn, "/wind", `{"id":"x"}`); code != codes.BadRequest {
		t.Fatalf("codice %v", code)
	}
	if c.count() != 0 {
		t.Fatalf("righe: %v", c.lines)
	}
}

func TestUnknownPath(t *testing.T) {
	c := &collected{}
	conn := dial(t, startServer(t, c))
	if code := postCON(t, conn, "/altro", validPayload); code != codes.NotFound {
		t.Fatalf("codice %v", code)
	}
}

func TestGetNotAllowed(t *testing.T) {
	c := &collected{}
	conn := dial(t, startServer(t, c))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := conn.Get(ctx, "/wind")
	if err != nil {
		t.Fatal(err)
	}
	if resp.Code() != codes.MethodNotAllowed {
		t.Fatalf("codice %v", resp.Code())
	}
}

// nonDatagram costruisce a mano un POST NON come quello del modem
func nonDatagram(t *testing.T, messageID int32, payload string) []byte {
	t.Helper()
	m := pool.NewMessage(context.Background())
	m.SetCode(codes.POST)
	m.SetType(message.NonConfirmable)
	m.SetMessageID(messageID)
	if err := m.SetPath("/wind"); err != nil {
		t.Fatal(err)
	}
	m.SetContentFormat(message.AppJSON)
	m.SetBody(bytes.NewReader([]byte(payload)))
	data, err := m.MarshalWithEncoder(coder.DefaultCoder)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func sendRaw(t *testing.T, addr string, datagrams ...[]byte) *net.UDPConn {
	t.Helper()
	remote, err := net.ResolveUDPAddr("udp4", addr)
	if err != nil {
		t.Fatal(err)
	}
	conn, err := net.DialUDP("udp4", nil, remote)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	for _, d := range datagrams {
		if _, err := conn.Write(d); err != nil {
			t.Fatal(err)
		}
	}
	return conn
}

func TestNonConfirmableGetsNoResponse(t *testing.T) {
	c := &collected{}
	addr := startServer(t, c)
	conn := sendRaw(t, addr, nonDatagram(t, 1000, validPayload))
	waitFor(t, func() bool { return c.count() == 1 })

	_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	buf := make([]byte, 64)
	if n, err := conn.Read(buf); err == nil {
		t.Fatalf("risposta inattesa: %x", buf[:n])
	}
}

// Un messaggio duplicato dalla rete non deve produrre due punti
func TestDuplicateMessageStoredOnce(t *testing.T) {
	c := &collected{}
	addr := startServer(t, c)
	d := nonDatagram(t, 2000, validPayload)
	sendRaw(t, addr, d, d)
	time.Sleep(300 * time.Millisecond)
	if c.count() != 1 {
		t.Fatalf("punti: %d", c.count())
	}
}

func TestDeduplicatorExpires(t *testing.T) {
	d := NewDeduplicator()
	start := time.Unix(1000, 0)
	if d.Seen("a", 1, start) {
		t.Error("primo messaggio visto come duplicato")
	}
	if !d.Seen("a", 1, start.Add(exchangeLifetime-time.Second)) {
		t.Error("duplicato non riconosciuto")
	}
	if d.Seen("b", 1, start) {
		t.Error("stesso ID da un altro indirizzo visto come duplicato")
	}
	if d.Seen("a", 1, start.Add(exchangeLifetime)) {
		t.Error("ID riusato dopo la scadenza visto come duplicato")
	}
	if len(d.seen) != 1 { // restano solo "a" appena registrato; "b" è scaduto
		t.Errorf("voci non rimosse: %d", len(d.seen))
	}
}

func TestDifferentMessageIDsBothStored(t *testing.T) {
	c := &collected{}
	addr := startServer(t, c)
	sendRaw(t, addr, nonDatagram(t, 3000, validPayload), nonDatagram(t, 3001, validPayload))
	waitFor(t, func() bool { return c.count() == 2 })
}
