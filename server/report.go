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
}

// ParseReport valida il payload JSON {"id":..., "mean":..., "max":..., "dir":..., "n":...},
// dove n è l'ora Unix di misura. Firma e ora vengono controllate da Auth.
func ParseReport(payload []byte) (Report, uint32, error) {
	var raw struct {
		ID   *string  `json:"id"`
		Mean *float64 `json:"mean"`
		Max  *float64 `json:"max"`
		Dir  *int     `json:"dir"`
		N    *uint32  `json:"n"`
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
	return Report{
		Name:       *raw.ID,
		Mean:       *raw.Mean,
		Max:        *raw.Max,
		Direction:  *raw.Dir,
		MeasuredAt: time.Unix(int64(*raw.N), 0),
	}, *raw.N, nil
}

// LineProtocol scrive lo stesso formato di collector_coap.py, con l'orario in secondi.
// Il nome non richiede escape: namePattern ammette solo caratteri sicuri.
func (r Report) LineProtocol() string {
	return fmt.Sprintf("meteo,sensor_name=%s wind_mean=%s,wind_max=%s,direction_deg=%di %d",
		r.Name,
		strconv.FormatFloat(r.Mean, 'f', -1, 64),
		strconv.FormatFloat(r.Max, 'f', -1, 64),
		r.Direction,
		r.MeasuredAt.Unix())
}
