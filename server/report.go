package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"time"
)

var namePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,32}$`)

// Report è il messaggio inviato dalla stazione ogni 5 minuti.
type Report struct {
	Name      string
	Mean      float64
	Max       float64
	Direction int
	// MeasuredAt è la fine del periodo di misura, secondo l'orologio della stazione.
	MeasuredAt time.Time
	// Posizione GNSS, presente solo nel messaggio dopo una nuova lettura (circa una volta a settimana).
	HasPosition bool
	Lat, Lon    float64
}

// ParseReport valida il payload JSON {"id":..., "mean":..., "max":..., "dir":..., "n":...},
// dove n è l'ora Unix di misura, con "lat" e "lon" facoltativi (sempre in coppia).
// Firma e ora vengono controllate da Auth.
func ParseReport(payload []byte) (Report, uint32, error) {
	var raw struct {
		ID   *string  `json:"id"`
		Mean *float64 `json:"mean"`
		Max  *float64 `json:"max"`
		Dir  *int     `json:"dir"`
		N    *uint32  `json:"n"`
		Lat  *float64 `json:"lat"`
		Lon  *float64 `json:"lon"`
	}
	if err := json.Unmarshal(payload, &raw); err != nil {
		return Report{}, 0, err
	}
	if raw.ID == nil || raw.Mean == nil || raw.Max == nil || raw.Dir == nil || raw.N == nil {
		return Report{}, 0, errors.New("campi mancanti")
	}
	if !namePattern.MatchString(*raw.ID) {
		return Report{}, 0, errors.New("nome stazione non valido")
	}
	if *raw.Mean < 0 || *raw.Max < 0 {
		return Report{}, 0, errors.New("velocità negativa")
	}
	if *raw.Dir < 0 || *raw.Dir > 360 {
		return Report{}, 0, errors.New("direzione fuori intervallo")
	}
	report := Report{
		Name:       *raw.ID,
		Mean:       *raw.Mean,
		Max:        *raw.Max,
		Direction:  *raw.Dir,
		MeasuredAt: time.Unix(int64(*raw.N), 0),
	}
	if (raw.Lat == nil) != (raw.Lon == nil) {
		return Report{}, 0, errors.New("posizione incompleta")
	}
	if raw.Lat != nil {
		if *raw.Lat < -90 || *raw.Lat > 90 || *raw.Lon < -180 || *raw.Lon > 180 {
			return Report{}, 0, errors.New("posizione fuori intervallo")
		}
		report.HasPosition, report.Lat, report.Lon = true, *raw.Lat, *raw.Lon
	}
	return report, *raw.N, nil
}

// LineProtocol scrive lo stesso formato di collector_coap.py, con l'orario in secondi.
// Con la posizione aggiunge una seconda riga nella measurement position.
// Il nome non richiede escape: namePattern ammette solo caratteri sicuri.
func (r Report) LineProtocol() string {
	line := fmt.Sprintf("meteo,sensor_name=%s wind_mean=%s,wind_max=%s,direction_deg=%di %d",
		r.Name,
		strconv.FormatFloat(r.Mean, 'f', -1, 64),
		strconv.FormatFloat(r.Max, 'f', -1, 64),
		r.Direction,
		r.MeasuredAt.Unix())
	if r.HasPosition {
		line += fmt.Sprintf("\nposition,sensor_name=%s lat=%s,lon=%s %d",
			r.Name,
			strconv.FormatFloat(r.Lat, 'f', -1, 64),
			strconv.FormatFloat(r.Lon, 'f', -1, 64),
			r.MeasuredAt.Unix())
	}
	return line
}
