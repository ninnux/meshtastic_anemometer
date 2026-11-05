package main

import (
	"testing"
	"time"
)

func TestParseReportValid(t *testing.T) {
	r, n, err := ParseReport([]byte(`{"id":"first_anemometer","mean":10.8,"max":15.1,"dir":225,"n":7}`))
	if err != nil {
		t.Fatal(err)
	}
	if n != 7 {
		t.Fatalf("contatore errato: %d", n)
	}
	if r != (Report{Name: "first_anemometer", Mean: 10.8, Max: 15.1, Direction: 225, MeasuredAt: time.Unix(7, 0)}) {
		t.Fatalf("report errato: %+v", r)
	}
}

func TestParseReportInvalid(t *testing.T) {
	cases := map[string]string{
		"non json":           `not json`,
		"vuoto":              ``,
		"manca id":           `{"mean":1,"max":1,"dir":1,"n":1}`,
		"manca mean":         `{"id":"a","max":1,"dir":1,"n":1}`,
		"manca max":          `{"id":"a","mean":1,"dir":1,"n":1}`,
		"manca dir":          `{"id":"a","mean":1,"max":1,"n":1}`,
		"manca n":            `{"id":"a","mean":1,"max":1,"dir":1}`,
		"n negativo":         `{"id":"a","mean":1,"max":1,"dir":1,"n":-1}`,
		"n troppo grande":    `{"id":"a","mean":1,"max":1,"dir":1,"n":4294967296}`,
		"formato a finestre": `{"id":"a","mean":[1,2],"max":1,"dir":1,"n":1}`,
		"nome con spazio":    `{"id":"a b","mean":1,"max":1,"dir":1,"n":1}`,
		"nome con virgola":   `{"id":"a,b=c","mean":1,"max":1,"dir":1,"n":1}`,
		"nome con a capo":    "{\"id\":\"a\\nb\",\"mean\":1,\"max\":1,\"dir\":1,\"n\":1}",
		"nome troppo lungo":  `{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","mean":1,"max":1,"dir":1,"n":1}`,
		"mean negativa":      `{"id":"a","mean":-1,"max":1,"dir":1,"n":1}`,
		"max negativa":       `{"id":"a","mean":1,"max":-1,"dir":1,"n":1}`,
		"dir negativa":       `{"id":"a","mean":1,"max":1,"dir":-1,"n":1}`,
		"dir fuori":          `{"id":"a","mean":1,"max":1,"dir":361,"n":1}`,
		"dir decimale":       `{"id":"a","mean":1,"max":1,"dir":1.5,"n":1}`,
		"NaN":                `{"id":"a","mean":NaN,"max":1,"dir":1,"n":1}`,
	}
	for name, payload := range cases {
		if _, _, err := ParseReport([]byte(payload)); err == nil {
			t.Errorf("%s: atteso errore", name)
		}
	}
}

func TestParseReportLimits(t *testing.T) {
	for _, payload := range []string{
		`{"id":"a","mean":0,"max":0,"dir":0,"n":0}`,
		`{"id":"a_b-C9","mean":1,"max":1,"dir":360,"n":4294967295}`,
	} {
		if _, _, err := ParseReport([]byte(payload)); err != nil {
			t.Errorf("%s: %v", payload, err)
		}
	}
}

func TestLineProtocol(t *testing.T) {
	r := Report{Name: "s1", Mean: 10.8, Max: 15, Direction: 225, MeasuredAt: time.Unix(1700000000, 0)}
	got := r.LineProtocol()
	want := "meteo,sensor_name=s1 wind_mean=10.8,wind_max=15,direction_deg=225i 1700000000"
	if got != want {
		t.Fatalf("\n got %s\nwant %s", got, want)
	}
}
