package main

import (
	"context"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	historyCacheTTL  = time.Minute
	stationsCacheTTL = 5 * time.Minute
	historyTimeout   = 30 * time.Second
	historyCacheMax  = 64
	stationsLookback = 365 * 24 * time.Hour
	positionLookback = 10 * 365 * 24 * time.Hour
)

// historyRange è un periodo selezionabile: i report vengono raggruppati in finestre
// di durata window, così anche un anno resta di qualche centinaio di punti.
type historyRange struct {
	span   time.Duration
	window time.Duration
}

var historyRanges = map[string]historyRange{
	"1h":   {time.Hour, 5 * time.Minute},
	"24h":  {24 * time.Hour, 5 * time.Minute},
	"7d":   {7 * 24 * time.Hour, 30 * time.Minute},
	"30d":  {30 * 24 * time.Hour, 2 * time.Hour},
	"365d": {365 * 24 * time.Hour, 24 * time.Hour},
}

// HistoryPoint è una finestra di report: media delle medie, massimo delle raffiche,
// direzione media (circolare) in gradi. Time è la fine della finestra.
type HistoryPoint struct {
	Time int64   `json:"t"`
	Mean float64 `json:"mean"`
	Max  float64 `json:"max"`
	Dir  int     `json:"dir"`
}

type cacheEntry struct {
	at   time.Time
	body []byte
}

// History serve lo storico letto da InfluxDB:
//
//	GET /api/history?station=nome&range=1h|24h|7d|30d|365d  (predefinito 24h)
//	GET /api/stations                                    elenco delle stazioni
//	GET /api/position?station=nome                       ultima posizione GNSS
//
// Il token di InfluxDB resta sul server: il browser vede solo questi dati.
type History struct {
	queryURL string
	token    string
	bucket   string
	client   *http.Client
	now      func() time.Time

	mu    sync.Mutex
	cache map[string]cacheEntry
}

func NewHistory(baseURL, token, org, bucket string) *History {
	return &History{
		queryURL: strings.TrimRight(baseURL, "/") + "/api/v2/query?" + url.Values{"org": {org}}.Encode(),
		token:    token,
		bucket:   bucket,
		client:   &http.Client{Timeout: historyTimeout},
		now:      time.Now,
		cache:    map[string]cacheEntry{},
	}
}

// Handler espone /api/history e /api/stations.
func (h *History) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/history", h.serveHistory)
	mux.HandleFunc("/api/stations", h.serveStations)
	mux.HandleFunc("/api/position", h.servePosition)
	return mux
}

func (h *History) serveHistory(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "metodo non ammesso", http.StatusMethodNotAllowed)
		return
	}
	station := r.URL.Query().Get("station")
	if !namePattern.MatchString(station) {
		http.Error(w, "stazione non valida", http.StatusBadRequest)
		return
	}
	rangeName := r.URL.Query().Get("range")
	if rangeName == "" {
		rangeName = "24h"
	}
	rng, ok := historyRanges[rangeName]
	if !ok {
		http.Error(w, "periodo non valido", http.StatusBadRequest)
		return
	}

	body, err := h.cached("h:"+rangeName+":"+station, historyCacheTTL, func() (any, error) {
		points, err := h.queryHistory(r.Context(), station, rng)
		return struct {
			Station string         `json:"station"`
			Range   string         `json:"range"`
			Window  int64          `json:"window"`
			Points  []HistoryPoint `json:"points"`
		}{station, rangeName, int64(rng.window.Seconds()), points}, err
	})
	h.respond(w, "storico "+station, body, err)
}

func (h *History) serveStations(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "metodo non ammesso", http.StatusMethodNotAllowed)
		return
	}
	body, err := h.cached("stations", stationsCacheTTL, func() (any, error) {
		names, err := h.queryStations(r.Context())
		return struct {
			Stations []string `json:"stations"`
		}{names}, err
	})
	h.respond(w, "elenco stazioni", body, err)
}

// LastPosition è l'ultima posizione GNSS di una stazione; Time è 0 se non ce n'è nessuna.
type LastPosition struct {
	Time int64   `json:"t"`
	Lat  float64 `json:"lat"`
	Lon  float64 `json:"lon"`
}

func (h *History) servePosition(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "metodo non ammesso", http.StatusMethodNotAllowed)
		return
	}
	station := r.URL.Query().Get("station")
	if !namePattern.MatchString(station) {
		http.Error(w, "stazione non valida", http.StatusBadRequest)
		return
	}
	body, err := h.cached("p:"+station, stationsCacheTTL, func() (any, error) {
		pos, err := h.queryPosition(r.Context(), station)
		return struct {
			Station  string        `json:"station"`
			Position *LastPosition `json:"position"`
		}{station, pos}, err
	})
	h.respond(w, "posizione "+station, body, err)
}

func (h *History) queryPosition(ctx context.Context, station string) (*LastPosition, error) {
	flux := fmt.Sprintf(`from(bucket: %q)
  |> range(start: -%ds)
  |> filter(fn: (r) => r._measurement == "position" and r.sensor_name == %q)
  |> filter(fn: (r) => r._field == "lat" or r._field == "lon")
  |> last()
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")`,
		h.bucket, int64(positionLookback.Seconds()), station)

	body, err := h.query(ctx, flux)
	if err != nil {
		return nil, err
	}
	defer body.Close()
	rows, err := readCSV(body)
	if err != nil {
		return nil, err
	}
	// lat e lon vengono sempre scritti insieme: si prende la riga più recente completa
	var last *LastPosition
	for _, row := range rows {
		t, err := time.Parse(time.RFC3339Nano, row["_time"])
		if err != nil {
			continue
		}
		lat, errLat := strconv.ParseFloat(row["lat"], 64)
		lon, errLon := strconv.ParseFloat(row["lon"], 64)
		if errLat != nil || errLon != nil {
			continue
		}
		if last == nil || t.Unix() > last.Time {
			last = &LastPosition{t.Unix(), lat, lon}
		}
	}
	return last, nil
}

func (h *History) respond(w http.ResponseWriter, what string, body []byte, err error) {
	if err != nil {
		log.Printf("%s: %v", what, err)
		http.Error(w, "storico non disponibile", http.StatusBadGateway)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache")
	w.Write(body)
}

// cached evita di interrogare InfluxDB a ogni apertura della pagina: i dati cambiano
// solo ogni 5 minuti.
func (h *History) cached(key string, ttl time.Duration, load func() (any, error)) ([]byte, error) {
	h.mu.Lock()
	entry, ok := h.cache[key]
	h.mu.Unlock()
	if ok && h.now().Sub(entry.at) < ttl {
		return entry.body, nil
	}

	value, err := load()
	if err != nil {
		return nil, err
	}
	body, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}

	h.mu.Lock()
	// Ogni nome valido crea una voce, anche se la stazione non esiste: limite alla memoria
	if len(h.cache) >= historyCacheMax {
		h.cache = map[string]cacheEntry{}
	}
	h.cache[key] = cacheEntry{h.now(), body}
	h.mu.Unlock()
	return body, nil
}

func (h *History) queryHistory(ctx context.Context, station string, rng historyRange) ([]HistoryPoint, error) {
	// station e bucket sono ammessi solo con caratteri sicuri: nessun escape necessario.
	// La direzione si media come vettore (seno e coseno): la media di 350° e 10° è 0°, non 180°.
	flux := fmt.Sprintf(`import "math"
base = from(bucket: %q)
  |> range(start: -%ds)
  |> filter(fn: (r) => r._measurement == "meteo" and r.sensor_name == %q)
avgs = base |> filter(fn: (r) => r._field == "wind_mean") |> toFloat() |> aggregateWindow(every: %[4]ds, fn: mean, createEmpty: false)
peaks = base |> filter(fn: (r) => r._field == "wind_max") |> toFloat() |> aggregateWindow(every: %[4]ds, fn: max, createEmpty: false)
dirs = base |> filter(fn: (r) => r._field == "direction_deg") |> toFloat()
dsin = dirs |> map(fn: (r) => ({r with _field: "dir_sin", _value: math.sin(x: r._value * math.pi / 180.0)})) |> aggregateWindow(every: %[4]ds, fn: mean, createEmpty: false)
dcos = dirs |> map(fn: (r) => ({r with _field: "dir_cos", _value: math.cos(x: r._value * math.pi / 180.0)})) |> aggregateWindow(every: %[4]ds, fn: mean, createEmpty: false)
union(tables: [avgs, peaks, dsin, dcos])
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> group()
  |> sort(columns: ["_time"])`, h.bucket, int64(rng.span.Seconds()), station, int64(rng.window.Seconds()))

	body, err := h.query(ctx, flux)
	if err != nil {
		return nil, err
	}
	defer body.Close()
	return parseHistoryCSV(body)
}

func (h *History) queryStations(ctx context.Context) ([]string, error) {
	flux := fmt.Sprintf(`import "influxdata/influxdb/schema"
schema.tagValues(bucket: %q, tag: "sensor_name", predicate: (r) => r._measurement == "meteo", start: -%ds)`,
		h.bucket, int64(stationsLookback.Seconds()))

	body, err := h.query(ctx, flux)
	if err != nil {
		return nil, err
	}
	defer body.Close()
	rows, err := readCSV(body)
	if err != nil {
		return nil, err
	}
	names := []string{}
	for _, row := range rows {
		if name := row["_value"]; namePattern.MatchString(name) {
			names = append(names, name)
		}
	}
	return names, nil
}

func (h *History) query(ctx context.Context, flux string) (io.ReadCloser, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, h.queryURL, strings.NewReader(flux))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Token "+h.token)
	req.Header.Set("Content-Type", "application/vnd.flux")
	req.Header.Set("Accept", "application/csv")

	resp, err := h.client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 300 {
		msg, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		resp.Body.Close()
		return nil, fmt.Errorf("HTTP %d: %s", resp.StatusCode, msg)
	}
	return resp.Body, nil
}

// readCSV legge il CSV annotato di InfluxDB come righe nome_colonna → valore. Ogni
// tabella ha la sua intestazione, separata dalla precedente da una riga vuota.
func readCSV(r io.Reader) ([]map[string]string, error) {
	reader := csv.NewReader(r)
	reader.FieldsPerRecord = -1
	var header []string
	rows := []map[string]string{}
	for {
		record, err := reader.Read()
		if errors.Is(err, io.EOF) {
			return rows, nil
		}
		if err != nil {
			return nil, err
		}
		if len(record) == 0 || (len(record) == 1 && record[0] == "") || strings.HasPrefix(record[0], "#") {
			continue
		}
		if header == nil || (len(record) > 1 && record[1] == "result") {
			header = record
			continue
		}
		row := map[string]string{}
		for i, name := range header {
			if i < len(record) {
				row[name] = record[i]
			}
		}
		rows = append(rows, row)
	}
}

func parseHistoryCSV(r io.Reader) ([]HistoryPoint, error) {
	rows, err := readCSV(r)
	if err != nil {
		return nil, err
	}
	points := []HistoryPoint{}
	for _, row := range rows {
		t, err := time.Parse(time.RFC3339Nano, row["_time"])
		if err != nil {
			return nil, fmt.Errorf("orario %q: %w", row["_time"], err)
		}
		point := HistoryPoint{Time: t.Unix()}
		// Un campo mancante resta a zero: meglio un punto incompleto che nessun grafico
		point.Mean, _ = strconv.ParseFloat(row["wind_mean"], 64)
		point.Max, _ = strconv.ParseFloat(row["wind_max"], 64)
		sin, _ := strconv.ParseFloat(row["dir_sin"], 64)
		cos, _ := strconv.ParseFloat(row["dir_cos"], 64)
		point.Dir = (int(math.Round(math.Atan2(sin, cos)*180/math.Pi)) + 360) % 360
		points = append(points, point)
	}
	return points, nil
}
