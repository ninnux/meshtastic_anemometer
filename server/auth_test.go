package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Generato dal firmware e calcolato anche con hmac di Python: vedi test_sim7080.cpp
const firmwareVector = `{"id":"first_anemometer","mean":10.8,"max":15.1,"dir":225,"n":65537,"sig":"3a482c440614dcc4d24ea0c6a99eed20"}`

func TestVerifyFirmwareVector(t *testing.T) {
	report, err := newTestAuth(t).Verify([]byte(firmwareVector))
	if err != nil {
		t.Fatal(err)
	}
	if report != (Report{Name: "first_anemometer", Mean: 10.8, Max: 15.1, Direction: 225}) {
		t.Fatalf("report errato: %+v", report)
	}
}

// Ogni modifica a un messaggio firmato deve invalidarlo
func TestVerifyRejectsTampering(t *testing.T) {
	cases := map[string]string{
		"velocità":  strings.Replace(firmwareVector, "10.8", "99.9", 1),
		"direzione": strings.Replace(firmwareVector, "225", "226", 1),
		"contatore": strings.Replace(firmwareVector, "65537", "65538", 1),
		"spazio":    strings.Replace(firmwareVector, `"mean":10.8`, `"mean": 10.8`, 1),
		"firma":     strings.Replace(firmwareVector, "3a482c440614dcc4d24ea0c6a99eed20", "00000000000000000000000000000000", 1),
		"stazione":  strings.Replace(firmwareVector, "first_anemometer", "altra", 1),
	}
	for name, payload := range cases {
		if _, err := newTestAuth(t).Verify([]byte(payload)); !errors.Is(err, errUnauthorized) {
			t.Errorf("%s: errore %v", name, err)
		}
	}
}

func TestVerifyMalformedSignature(t *testing.T) {
	cases := map[string]string{
		"senza firma":   `{"id":"first_anemometer","mean":1,"max":1,"dir":1,"n":1}`,
		"firma corta":   `{"id":"first_anemometer","mean":1,"max":1,"dir":1,"n":1,"sig":"abcd"}`,
		"firma maiusc.": strings.ToUpper(firmwareVector),
		"campo dopo":    strings.TrimSuffix(firmwareVector, "}") + `,"x":"y"}`,
		"non json":      `xx,"sig":"3a482c440614dcc4d24ea0c6a99eed20"}`,
	}
	for name, payload := range cases {
		if _, err := newTestAuth(t).Verify([]byte(payload)); !errors.Is(err, errMalformed) {
			t.Errorf("%s: errore %v", name, err)
		}
	}
}

func TestVerifyRejectsOldCounters(t *testing.T) {
	a := newTestAuth(t)
	for _, step := range []struct {
		counter int
		ok      bool
	}{{10, true}, {10, false}, {9, false}, {11, true}, {1000000, true}, {500, false}} {
		_, err := a.Verify([]byte(signedPayload(testKey, step.counter)))
		if step.ok && err != nil {
			t.Errorf("contatore %d: %v", step.counter, err)
		}
		if !step.ok && !errors.Is(err, errUnauthorized) {
			t.Errorf("contatore %d: errore %v", step.counter, err)
		}
	}
}

// Un messaggio falso non deve far avanzare il contatore, altrimenti bloccherebbe la stazione
func TestRejectedMessageDoesNotAdvanceCounter(t *testing.T) {
	a := newTestAuth(t)
	if _, err := a.Verify([]byte(signedPayload("chiave-falsa-chiave-falsa-chiave-falsa", 1000))); err == nil {
		t.Fatal("firma falsa accettata")
	}
	if _, err := a.Verify([]byte(signedPayload(testKey, 1))); err != nil {
		t.Fatal(err)
	}
}

func signedBy(name, key string, counter int) []byte {
	prefix := fmt.Sprintf(`{"id":"%s","mean":1,"max":2,"dir":3,"n":%d`, name, counter)
	mac := hmac.New(sha256.New, []byte(key))
	mac.Write([]byte(prefix))
	return []byte(prefix + `,"sig":"` + hex.EncodeToString(mac.Sum(nil)[:signatureBytes]) + `"}`)
}

// Ogni nodo ha la sua chiave e il suo contatore
func TestStationsAreIndependent(t *testing.T) {
	keyA, keyB := strings.Repeat("a", minKeyLength), strings.Repeat("b", minKeyLength)
	a, err := NewAuth(map[string][]byte{"nodo_a": []byte(keyA), "nodo_b": []byte(keyB)}, "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := a.Verify(signedBy("nodo_a", keyA, 1000)); err != nil {
		t.Fatal(err)
	}
	// Il contatore alto di un nodo non blocca l'altro
	if _, err := a.Verify(signedBy("nodo_b", keyB, 1)); err != nil {
		t.Fatal(err)
	}
	// Chi possiede la chiave di un nodo non può scrivere a nome dell'altro
	if _, err := a.Verify(signedBy("nodo_b", keyA, 2)); !errors.Is(err, errUnauthorized) {
		t.Fatalf("chiave di nodo_a accettata per nodo_b: %v", err)
	}
	// Un nodo non presente in STATION_KEYS viene rifiutato
	if _, err := a.Verify(signedBy("nodo_c", keyA, 1)); !errors.Is(err, errUnauthorized) {
		t.Fatalf("nodo sconosciuto accettato: %v", err)
	}
}

func TestCountersSurviveRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "counters.json")
	keys := map[string][]byte{"first_anemometer": []byte(testKey)}
	first, err := NewAuth(keys, path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := first.Verify([]byte(signedPayload(testKey, 42))); err != nil {
		t.Fatal(err)
	}

	second, err := NewAuth(keys, path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := second.Verify([]byte(signedPayload(testKey, 42))); !errors.Is(err, errUnauthorized) {
		t.Fatalf("messaggio riusato dopo il riavvio: %v", err)
	}
	if _, err := second.Verify([]byte(signedPayload(testKey, 43))); err != nil {
		t.Fatal(err)
	}
	entries, _ := os.ReadDir(filepath.Dir(path))
	if len(entries) != 1 {
		t.Fatalf("file temporanei rimasti: %v", entries)
	}
}

func TestCorruptStateRefusesToStart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "counters.json")
	if err := os.WriteFile(path, []byte("{rotto"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewAuth(nil, path); err == nil {
		t.Fatal("contatori corrotti accettati")
	}
}

func TestParseStationKeys(t *testing.T) {
	long := strings.Repeat("k", minKeyLength)
	keys, err := ParseStationKeys("a:" + long + " , b_2:" + long + "x,")
	if err != nil {
		t.Fatal(err)
	}
	if len(keys) != 2 || string(keys["a"]) != long || string(keys["b_2"]) != long+"x" {
		t.Fatalf("chiavi: %v", keys)
	}
	if keys, err := ParseStationKeys(""); err != nil || len(keys) != 0 {
		t.Fatalf("vuoto: %v %v", keys, err)
	}
	for _, spec := range []string{"senza-chiave", "a:corta", ":" + long, "a b:" + long, "a:" + long + ",a:" + long} {
		if _, err := ParseStationKeys(spec); err == nil {
			t.Errorf("%q: atteso errore", spec)
		}
	}
}
