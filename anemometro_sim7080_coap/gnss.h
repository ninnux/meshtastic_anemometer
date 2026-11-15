// ============================================================
// gnss.h – Posizione letta dal GNSS del SIM7080G. Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stdlib.h>
#include <string.h>

struct GnssPosition {
  float lat;  // gradi, positivi a nord
  float lon;  // gradi, positivi a est
};

// Legge la risposta di AT+CGNSINF dopo "+CGNSINF:", senza spazi:
// <run>,<fix>,<UTC>,<lat>,<lon>,<alt>,...
// Ritorna true solo con il fix valido e coordinate plausibili.
inline bool parseGnssInfo(const char* text, GnssPosition& pos) {
  const char* field[5];
  field[0] = text;
  for (int i = 1; i < 5; i++) {
    const char* comma = strchr(field[i - 1], ',');
    if (!comma) return false;
    field[i] = comma + 1;
  }
  if (field[1][0] != '1' || field[1][1] != ',') return false;  // fix non ancora valido

  char* end;
  double lat = strtod(field[3], &end);
  if (end == field[3] || *end != ',') return false;
  double lon = strtod(field[4], &end);
  if (end == field[4] || (*end != ',' && *end != '\0')) return false;

  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  if (lat == 0 && lon == 0) return false;  // valore di riempimento di alcuni moduli
  pos.lat = (float)lat;
  pos.lon = (float)lon;
  return true;
}
