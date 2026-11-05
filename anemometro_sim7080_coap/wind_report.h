// ============================================================
// wind_report.h – Report con i valori del periodo di invio.
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdio.h>

struct WindReport {
  float mean;
  float max;
  int   dir;
};

// Scrive il payload JSON in out. Ritorna false se non entra in cap.
inline bool formatWindPayload(char* out, size_t cap, const char* id, const WindReport& r) {
  int written = snprintf(out, cap, "{\"id\":\"%s\",\"mean\":%.1f,\"max\":%.1f,\"dir\":%d}",
                         id, r.mean, r.max, r.dir);
  return written >= 0 && (size_t)written < cap;
}
