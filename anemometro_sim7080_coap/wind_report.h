// ============================================================
// wind_report.h – Report al minuto con una terna di valori per
// ogni finestra da 10 s. Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdio.h>

#define WIND_WINDOWS 6

struct WindReport {
  float mean[WIND_WINDOWS];
  float max[WIND_WINDOWS];
  int   dir[WIND_WINDOWS];
};

// Scrive il payload JSON in out. Ritorna false se non entra in cap.
inline bool formatWindPayload(char* out, size_t cap, const char* id, const WindReport& r) {
  int written = snprintf(out, cap,
    "{\"id\":\"%s\","
    "\"mean\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],"
    "\"max\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],"
    "\"dir\":[%d,%d,%d,%d,%d,%d]}",
    id,
    r.mean[0], r.mean[1], r.mean[2], r.mean[3], r.mean[4], r.mean[5],
    r.max[0],  r.max[1],  r.max[2],  r.max[3],  r.max[4],  r.max[5],
    r.dir[0],  r.dir[1],  r.dir[2],  r.dir[3],  r.dir[4],  r.dir[5]);
  return written >= 0 && (size_t)written < cap;
}
