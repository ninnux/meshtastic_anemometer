// Ricevitore CoAP per anemometro_sim7080_coap: POST su /wind con payload JSON
// {"id": nome_stazione, "mean": kn, "max": kn, "dir": gradi}.
//
// Senza INFLUX_URL nell'ambiente registra soltanto i dati ricevuti.
package main

import (
	"bytes"
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/mux"
	coapnet "github.com/plgd-dev/go-coap/v3/net"
	"github.com/plgd-dev/go-coap/v3/options"
	"github.com/plgd-dev/go-coap/v3/udp"
)

const maxLoggedPayload = 200

func getenv(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

// reply risponde solo ai messaggi CON: ai NON una risposta consumerebbe solo
// traffico dati sulla SIM, perché il modem non la legge.
func reply(w mux.ResponseWriter, r *mux.Message, code codes.Code) {
	if r.Type() != message.Confirmable {
		return
	}
	if err := w.SetResponse(code, message.TextPlain, bytes.NewReader(nil)); err != nil {
		log.Printf("risposta CoAP non impostata: %v", err)
	}
}

func windHandler(store func(line string)) mux.HandlerFunc {
	dedup := NewDeduplicator()
	return func(w mux.ResponseWriter, r *mux.Message) {
		remote := w.Conn().RemoteAddr()
		if r.Code() != codes.POST {
			reply(w, r, codes.MethodNotAllowed)
			return
		}
		if dedup.Seen(remote.String(), r.MessageID(), time.Now()) {
			reply(w, r, codes.Changed)
			return
		}

		payload, err := r.ReadBody()
		if err == nil {
			var report Report
			if report, err = ParseReport(payload); err == nil {
				if store != nil {
					store(report.LineProtocol(time.Now()))
				}
				log.Printf("ricevuto %s: media %gkn max %gkn dir %d", report.Name, report.Mean, report.Max, report.Direction)
				reply(w, r, codes.Changed)
				return
			}
		}

		if len(payload) > maxLoggedPayload {
			payload = payload[:maxLoggedPayload]
		}
		log.Printf("messaggio non valido da %v: %v: %q", remote, err, payload)
		reply(w, r, codes.BadRequest)
	}
}

func newRouter(store func(line string)) (*mux.Router, error) {
	router := mux.NewRouter()
	if err := router.Handle("/wind", windHandler(store)); err != nil {
		return nil, err
	}
	return router, nil
}

func run() error {
	if path := os.Getenv("LOG_FILE"); path != "" {
		file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o640)
		if err != nil {
			return fmt.Errorf("apertura log: %w", err)
		}
		defer file.Close()
		log.SetOutput(file)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	var store func(string)
	writerDone := make(chan struct{})
	if influxURL := os.Getenv("INFLUX_URL"); influxURL != "" {
		writer := NewInfluxWriter(influxURL, os.Getenv("INFLUX_TOKEN"),
			getenv("INFLUX_ORG", "meteo_org"), getenv("INFLUX_BUCKET", "anemometri"))
		store = writer.Enqueue
		go func() {
			writer.Run(ctx)
			close(writerDone)
		}()
	} else {
		log.Print("INFLUX_URL non impostato: i dati vengono solo registrati nel log")
		close(writerDone)
	}

	router, err := newRouter(store)
	if err != nil {
		return err
	}

	address := net.JoinHostPort(getenv("COAP_BIND", "::"), getenv("COAP_PORT", "5683"))
	listener, err := coapnet.NewListenUDP("udp", address)
	if err != nil {
		return fmt.Errorf("ascolto su %s: %w", address, err)
	}
	defer listener.Close()

	server := udp.NewServer(options.WithMux(router))
	served := make(chan error, 1)
	go func() { served <- server.Serve(listener) }()
	log.Printf("server CoAP in ascolto su %s", address)

	select {
	case err = <-served:
	case <-ctx.Done():
		server.Stop()
		<-served
	}
	stop()
	<-writerDone
	log.Print("server CoAP fermato")
	return err
}

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}
