// ============================================================
// wind_report.h – Report con i valori del periodo di invio, firmato
// con HMAC-SHA256. Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hmac_sha256.h"

#define WIND_SIGNATURE_BYTES 16  // HMAC troncato a 128 bit

struct WindReport {
  float mean;
  float max;
  int   dir;
};

// Impulsi al secondo sugli ultimi 3 campioni, a finestra scorrevole: nessuna
// raffica viene spezzata a meta'. La finestra sopravvive agli invii.
class RollingGust {
public:
  // Ritorna false finche' la finestra non e' piena.
  bool add(uint32_t pulses, float elapsedS, float& pulsesPerSecond) {
    _pulses[_next] = pulses;
    _elapsedS[_next] = elapsedS;
    _next = (_next + 1) % SAMPLES;
    if (_count < SAMPLES) _count++;
    if (_count < SAMPLES) return false;

    uint32_t totalPulses = 0;
    float totalS = 0.0f;
    for (uint8_t i = 0; i < SAMPLES; i++) {
      totalPulses += _pulses[i];
      totalS += _elapsedS[i];
    }
    pulsesPerSecond = totalPulses / totalS;
    return true;
  }

private:
  static const uint8_t SAMPLES = 3;
  uint32_t _pulses[SAMPLES];
  float    _elapsedS[SAMPLES];
  uint8_t  _next  = 0;
  uint8_t  _count = 0;
};

// Contatore sempre crescente, anche dopo un riavvio: la parte alta (epoca) e' salvata
// in flash solo al primo invio dopo un avvio e dopo 65536 messaggi (circa 227 giorni).
class MessageCounter {
public:
  // Ultima epoca salvata in flash, 0 se non ce n'e' ancora una
  void restore(uint16_t lastEpoch) {
    _epoch = lastEpoch;
    _ready = false;
  }

  // Serve salvare nextEpoch() in flash prima di usare next()
  bool ready() const { return _ready; }

  // Non si riparte da zero: il server scarterebbe tutti i messaggi come gia' visti
  bool exhausted() const { return !_ready && _epoch == 0xFFFF; }

  uint16_t nextEpoch() const { return _epoch + 1; }

  void epochSaved() {
    _epoch = nextEpoch();
    _sequence = 0;
    _ready = true;
  }

  uint32_t next() {
    uint32_t value = ((uint32_t)_epoch << 16) | _sequence;
    if (++_sequence == 0) _ready = false;
    return value;
  }

private:
  uint16_t _epoch    = 0;
  uint16_t _sequence = 0;
  bool     _ready    = false;
};

// Scrive il payload JSON in out. La firma copre tutti i byte che la precedono.
// Ritorna false se non entra in cap.
inline bool formatSignedWindPayload(char* out, size_t cap, const char* id, const WindReport& r,
                                    uint32_t counter, const char* key) {
  int written = snprintf(out, cap, "{\"id\":\"%s\",\"mean\":%.1f,\"max\":%.1f,\"dir\":%d,\"n\":%lu",
                         id, r.mean, r.max, r.dir, (unsigned long)counter);
  // ,"sig":" + esadecimale + "} + terminatore
  const size_t tailLen = 8 + WIND_SIGNATURE_BYTES * 2 + 2 + 1;
  if (written < 0 || (size_t)written + tailLen > cap) return false;

  uint8_t digest[SHA256_DIGEST_BYTES];
  hmacSha256((const uint8_t*)key, strlen(key), (const uint8_t*)out, (size_t)written, digest);

  size_t n = (size_t)written;
  n += snprintf(out + n, cap - n, ",\"sig\":\"");
  for (int i = 0; i < WIND_SIGNATURE_BYTES; i++) n += snprintf(out + n, cap - n, "%02x", digest[i]);
  snprintf(out + n, cap - n, "\"}");
  return true;
}
