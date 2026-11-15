package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// Come lo restituisce InfluxDB dopo pivot: dir 180° (sin 0, cos -1) e 90° (sin 1, cos 0)
const historyCSV = ",result,table,_time,_measurement,_start,_stop,sensor_name,dir_cos,dir_sin,wind_max,wind_mean\r\n" +
	",_result,0,2026-11-15T12:50:00Z,meteo,2026-11-14T13:00:00Z,2026-11-15T13:00:00Z,st,-1,0,12.5,8.1\r\n" +
	",_result,0,2026-11-15T12:55:00Z,meteo,2026-11-14T13:00:00Z,2026-11-15T13:00:00Z,st,0,1,14,9.75\r\n" +
	"\r\n"

const stationsCSV = ",result,table,_value\r\n,_result,0,alfa\r\n,_result,0,beta\r\n\r\n"

type fakeQuery struct {
	mu     sync.Mutex
	calls  int
	status int
	flux   string
	auth   string
	body   string
}

func (f *fakeQuery) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	b, _ := io.ReadAll(r.Body)
	f.flux, f.auth = string(b), r.Header.Get("Authorization")
	if f.status != 0 {
		w.WriteHeader(f.status)
		return
	}
	io.WriteString(w, f.body)
}

func newTestHistory(t *testing.T, f *fakeQuery) (http.Handler, *time.Time) {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	h := NewHistory(srv.URL, "segreto", "org", "bucket")
	now := time.Unix(1_800_000_000, 0)
	h.now = func() time.Time { return now }
	return h.Handler(), &now
}

func get(t *testing.T, h http.Handler, target string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, target, nil))
	return rec
}

func TestHistoryReturnsPoints(t *testing.T) {
	f := &fakeQuery{body: historyCSV}
	h, _ := newTestHistory(t, f)
	rec := get(t, h, "/api/history?station=st")
	if rec.Code != http.StatusOK {
		t.Fatalf("codice %d: %s", rec.Code, rec.Body)
	}
	var got struct {
		Station, Range string
		Window         int64
		Points         []HistoryPoint
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	want := []HistoryPoint{
		{Time: time.Date(2026, 11, 15, 12, 50, 0, 0, time.UTC).Unix(), Mean: 8.1, Max: 12.5, Dir: 180},
		{Time: time.Date(2026, 11, 15, 12, 55, 0, 0, time.UTC).Unix(), Mean: 9.75, Max: 14, Dir: 90},
	}
	if got.Station != "st" || got.Range != "24h" || got.Window != 300 ||
		len(got.Points) != 2 || got.Points[0] != want[0] || got.Points[1] != want[1] {
		t.Fatalf("ricevuto %+v", got)
	}
	if f.auth != "Token segreto" {
		t.Errorf("auth: %q", f.auth)
	}
	for _, part := range []string{`from(bucket: "bucket")`, `r.sensor_name == "st"`, "range(start: -86400s)", "every: 300s"} {
		if !strings.Contains(f.flux, part) {
			t.Errorf("query senza %q:\n%s", part, f.flux)
		}
	}
}

func TestHistoryRanges(t *testing.T) {
	for name, want := range map[string][]string{
		"1h":   {"range(start: -3600s)", "every: 300s"},
		"7d":   {"range(start: -604800s)", "every: 1800s"},
		"30d":  {"range(start: -2592000s)", "every: 7200s"},
		"365d": {"range(start: -31536000s)", "every: 86400s"},
	} {
		f := &fakeQuery{body: historyCSV}
		h, _ := newTestHistory(t, f)
		if rec := get(t, h, "/api/history?station=st&range="+name); rec.Code != http.StatusOK {
			t.Fatalf("%s: codice %d", name, rec.Code)
		}
		for _, part := range want {
			if !strings.Contains(f.flux, part) {
				t.Errorf("%s: query senza %q", name, part)
			}
		}
	}
}

// Media vettoriale: vicino al nord non deve uscire un valore negativo o 360
func TestHistoryDirectionWrapsToRange(t *testing.T) {
	csv := ",result,table,_time,dir_cos,dir_sin,wind_max,wind_mean\r\n" +
		",_result,0,2026-11-15T12:50:00Z,0.98,-0.17,1,1\r\n"
	points, err := parseHistoryCSV(strings.NewReader(csv))
	if err != nil || len(points) != 1 || points[0].Dir != 350 {
		t.Fatalf("punti %+v, errore %v", points, err)
	}
}

func TestHistoryEmpty(t *testing.T) {
	h, _ := newTestHistory(t, &fakeQuery{body: "\r\n"})
	rec := get(t, h, "/api/history?station=st")
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"points":[]`) {
		t.Fatalf("codice %d: %s", rec.Code, rec.Body)
	}
}

// Il nome finisce nella query Flux: uno non valido non deve mai arrivare a InfluxDB
func TestHistoryRejectsBadInput(t *testing.T) {
	f := &fakeQuery{body: historyCSV}
	h, _ := newTestHistory(t, f)
	for _, target := range []string{"/api/history", `/api/history?station=a"b`,
		"/api/history?station=a%20b", "/api/history?station=st&range=2h"} {
		if rec := get(t, h, target); rec.Code != http.StatusBadRequest {
			t.Errorf("%s: codice %d", target, rec.Code)
		}
	}
	if f.calls != 0 {
		t.Fatalf("InfluxDB interrogato %d volte", f.calls)
	}
}

func TestHistoryCachesForAMinute(t *testing.T) {
	f := &fakeQuery{body: historyCSV}
	h, now := newTestHistory(t, f)
	get(t, h, "/api/history?station=st")
	get(t, h, "/api/history?station=st&range=24h")
	if f.calls != 1 {
		t.Fatalf("chiamate: %d", f.calls)
	}
	get(t, h, "/api/history?station=st&range=7d")
	if f.calls != 2 {
		t.Fatalf("un altro periodo deve fare una nuova query: %d", f.calls)
	}
	*now = now.Add(historyCacheTTL)
	get(t, h, "/api/history?station=st")
	if f.calls != 3 {
		t.Fatalf("chiamate dopo la scadenza: %d", f.calls)
	}
}

func TestHistoryInfluxError(t *testing.T) {
	h, _ := newTestHistory(t, &fakeQuery{status: http.StatusUnauthorized})
	for _, target := range []string{"/api/history?station=st", "/api/stations"} {
		rec := get(t, h, target)
		if rec.Code != http.StatusBadGateway {
			t.Fatalf("%s: codice %d", target, rec.Code)
		}
		if strings.Contains(rec.Body.String(), "segreto") {
			t.Fatal("il token non deve uscire nella risposta")
		}
	}
}

func TestPosition(t *testing.T) {
	f := &fakeQuery{body: ",result,table,_time,_measurement,sensor_name,lat,lon\r\n" +
		",_result,0,2026-11-15T10:00:00Z,position,st,41.89123,12.49234\r\n\r\n"}
	h, _ := newTestHistory(t, f)
	rec := get(t, h, "/api/position?station=st")
	want := `{"station":"st","position":{"t":1794736800,"lat":41.89123,"lon":12.49234}}`
	if rec.Code != http.StatusOK || strings.TrimSpace(rec.Body.String()) != want {
		t.Fatalf("codice %d: %s", rec.Code, rec.Body)
	}
	for _, part := range []string{`r._measurement == "position"`, `r.sensor_name == "st"`, "last()"} {
		if !strings.Contains(f.flux, part) {
			t.Errorf("query senza %q:\n%s", part, f.flux)
		}
	}
}

func TestPositionNone(t *testing.T) {
	h, _ := newTestHistory(t, &fakeQuery{body: "\r\n"})
	rec := get(t, h, "/api/position?station=st")
	if rec.Code != http.StatusOK || strings.TrimSpace(rec.Body.String()) != `{"station":"st","position":null}` {
		t.Fatalf("codice %d: %s", rec.Code, rec.Body)
	}
}

func TestPositionRejectsBadStation(t *testing.T) {
	f := &fakeQuery{}
	h, _ := newTestHistory(t, f)
	if rec := get(t, h, `/api/position?station=a"b`); rec.Code != http.StatusBadRequest || f.calls != 0 {
		t.Fatalf("codice %d, chiamate %d", rec.Code, f.calls)
	}
}

func TestStationsList(t *testing.T) {
	f := &fakeQuery{body: stationsCSV}
	h, _ := newTestHistory(t, f)
	rec := get(t, h, "/api/stations")
	if rec.Code != http.StatusOK || strings.TrimSpace(rec.Body.String()) != `{"stations":["alfa","beta"]}` {
		t.Fatalf("codice %d: %s", rec.Code, rec.Body)
	}
	if !strings.Contains(f.flux, `schema.tagValues(bucket: "bucket", tag: "sensor_name"`) {
		t.Errorf("query: %s", f.flux)
	}
	get(t, h, "/api/stations")
	if f.calls != 1 {
		t.Fatalf("chiamate: %d", f.calls)
	}
}
