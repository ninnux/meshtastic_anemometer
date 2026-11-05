package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
)

const (
	signatureBytes = 16 // HMAC-SHA256 troncato a 128 bit
	minKeyLength   = 32
	signatureField = `,"sig":"`
)

var (
	errMalformed     = errors.New("messaggio non valido")
	errUnauthorized  = errors.New("messaggio non autorizzato")
	signaturePattern = regexp.MustCompile(`^[0-9a-f]{32}$`)
)

// Auth verifica la firma HMAC e impedisce di riusare un messaggio già visto: ogni
// stazione ha la sua chiave e un contatore che deve sempre crescere. I contatori sono
// salvati su file, altrimenti dopo un riavvio del server i vecchi messaggi tornerebbero validi.
type Auth struct {
	keys      map[string][]byte
	statePath string

	mu       sync.Mutex
	counters map[string]uint32
}

// ParseStationKeys legge "nome:chiave,nome:chiave". Una chiave corta si indovina:
// meglio non partire che credersi protetti.
func ParseStationKeys(spec string) (map[string][]byte, error) {
	keys := make(map[string][]byte)
	for _, entry := range strings.Split(spec, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		name, key, found := strings.Cut(entry, ":")
		if !found || !namePattern.MatchString(name) {
			return nil, fmt.Errorf("voce di STATION_KEYS non valida per la stazione %q", name)
		}
		if len(key) < minKeyLength {
			return nil, fmt.Errorf("chiave della stazione %q troppo corta: servono almeno %d caratteri", name, minKeyLength)
		}
		if _, duplicate := keys[name]; duplicate {
			return nil, fmt.Errorf("stazione %q ripetuta in STATION_KEYS", name)
		}
		keys[name] = []byte(key)
	}
	return keys, nil
}

// NewAuth carica i contatori da statePath. Un file illeggibile è un errore: partire
// con i contatori azzerati riaprirebbe la porta ai vecchi messaggi.
func NewAuth(keys map[string][]byte, statePath string) (*Auth, error) {
	a := &Auth{keys: keys, statePath: statePath, counters: make(map[string]uint32)}
	if statePath == "" {
		return a, nil
	}
	data, err := os.ReadFile(statePath)
	if errors.Is(err, os.ErrNotExist) {
		return a, nil
	}
	if err != nil {
		return nil, fmt.Errorf("lettura contatori: %w", err)
	}
	if err := json.Unmarshal(data, &a.counters); err != nil {
		return nil, fmt.Errorf("contatori in %s non validi: %w", statePath, err)
	}
	return a, nil
}

// Verify ritorna il report solo se firma e contatore sono validi, e registra il contatore.
// La firma copre i byte esatti ricevuti prima del campo sig, così non dipende da come
// il firmware formatta i numeri.
func (a *Auth) Verify(payload []byte) (Report, error) {
	index := bytes.LastIndex(payload, []byte(signatureField))
	if index < 0 || !bytes.HasSuffix(payload, []byte(`"}`)) {
		return Report{}, fmt.Errorf("%w: firma mancante", errMalformed)
	}
	signed := payload[:index]
	signature := payload[index+len(signatureField) : len(payload)-2]
	if !signaturePattern.Match(signature) {
		return Report{}, fmt.Errorf("%w: firma malformata", errMalformed)
	}

	report, counter, err := ParseReport(payload)
	if err != nil {
		return Report{}, fmt.Errorf("%w: %v", errMalformed, err)
	}

	key, known := a.keys[report.Name]
	if !known {
		return Report{}, fmt.Errorf("%w: stazione %q sconosciuta", errUnauthorized, report.Name)
	}
	mac := hmac.New(sha256.New, key)
	mac.Write(signed)
	expected := make([]byte, hex.EncodedLen(signatureBytes))
	hex.Encode(expected, mac.Sum(nil)[:signatureBytes])
	if !hmac.Equal(expected, signature) {
		return Report{}, fmt.Errorf("%w: firma errata per %q", errUnauthorized, report.Name)
	}

	a.mu.Lock()
	defer a.mu.Unlock()
	if last := a.counters[report.Name]; counter <= last {
		return Report{}, fmt.Errorf("%w: contatore %d già visto per %q (ultimo %d)", errUnauthorized, counter, report.Name, last)
	}
	a.counters[report.Name] = counter
	if err := a.save(); err != nil {
		// Il dato è autentico e vale 5 minuti di misure: si tiene e si segnala
		log.Printf("contatori non salvati: %v", err)
	}
	return report, nil
}

// save scrive su un file temporaneo e poi rinomina: un guasto a metà non corrompe i contatori.
func (a *Auth) save() error {
	if a.statePath == "" {
		return nil
	}
	data, err := json.Marshal(a.counters)
	if err != nil {
		return err
	}
	temp, err := os.CreateTemp(filepath.Dir(a.statePath), ".counters-*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	if _, err := temp.Write(data); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	return os.Rename(temp.Name(), a.statePath)
}
