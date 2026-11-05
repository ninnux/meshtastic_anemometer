package main

import (
	"fmt"
	"sync"
	"time"
)

// exchangeLifetime è il tempo oltre il quale RFC 7252 consente di riusare un Message ID.
const exchangeLifetime = 247 * time.Second

// Deduplicator riconosce i messaggi duplicati dalla rete: go-coap filtra solo le
// ritrasmissioni CON, e un duplicato NON diventerebbe un secondo punto su InfluxDB.
type Deduplicator struct {
	mu   sync.Mutex
	seen map[string]time.Time
}

func NewDeduplicator() *Deduplicator {
	return &Deduplicator{seen: make(map[string]time.Time)}
}

// Seen registra il messaggio e ritorna true se era già stato visto di recente.
func (d *Deduplicator) Seen(remote string, messageID int32, now time.Time) bool {
	d.mu.Lock()
	defer d.mu.Unlock()

	for key, at := range d.seen {
		if now.Sub(at) >= exchangeLifetime {
			delete(d.seen, key)
		}
	}

	key := fmt.Sprintf("%s/%d", remote, messageID)
	if _, found := d.seen[key]; found {
		return true
	}
	d.seen[key] = now
	return false
}
