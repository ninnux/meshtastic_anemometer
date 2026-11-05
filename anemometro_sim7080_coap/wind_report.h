// ============================================================
// wind_report.h – Report con i valori del periodo di invio.
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

// Scrive il payload JSON in out. Ritorna false se non entra in cap.
inline bool formatWindPayload(char* out, size_t cap, const char* id, const WindReport& r) {
  int written = snprintf(out, cap, "{\"id\":\"%s\",\"mean\":%.1f,\"max\":%.1f,\"dir\":%d}",
                         id, r.mean, r.max, r.dir);
  return written >= 0 && (size_t)written < cap;
}
