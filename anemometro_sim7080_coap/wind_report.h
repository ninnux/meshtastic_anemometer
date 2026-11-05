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
  float    mean;
  float    max;
  int      dir;
  uint32_t closedTick;  // tick di FreeRTOS alla chiusura del periodo
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

// Scrive il payload JSON in out. La firma copre tutti i byte che la precedono.
// Ritorna false se non entra in cap.
inline bool formatSignedWindPayload(char* out, size_t cap, const char* id, const WindReport& r,
                                    uint32_t unixSeconds, const char* key) {
  int written = snprintf(out, cap, "{\"id\":\"%s\",\"mean\":%.1f,\"max\":%.1f,\"dir\":%d,\"n\":%lu",
                         id, r.mean, r.max, r.dir, (unsigned long)unixSeconds);
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
