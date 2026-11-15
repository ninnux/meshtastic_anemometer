// ============================================================
// gnss.h – Posizione letta dal GNSS del SIM7080G. Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stdlib.h>
#include <string.h>

#define GNSS_HPA_UNKNOWN 1e9f

struct GnssPosition {
  float lat;  // gradi, positivi a nord
  float lon;  // gradi, positivi a est
  float hpa;  // errore orizzontale stimato dal modem, metri (GNSS_HPA_UNKNOWN se manca)
};

// Legge la risposta di AT+CGNSINF dopo "+CGNSINF:", senza spazi:
// <run>,<fix>,<UTC>,<lat>,<lon>,<alt>,<speed>,<course>,<mode>,,<HDOP>,<PDOP>,<VDOP>,,
// <satelliti in vista>,,<HPA>,<VPA>  (manuale 8.2.2)
// Ritorna true solo con il fix valido e coordinate plausibili.
inline bool parseGnssInfo(const char* text, GnssPosition& pos) {
  const int HPA = 16;
  const char* field[HPA + 1] = {text};
  int count = 1;
  for (; count <= HPA; count++) {
    const char* comma = strchr(field[count - 1], ',');
    if (!comma) break;
    field[count] = comma + 1;
  }
  if (count < 5) return false;
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
  pos.hpa = GNSS_HPA_UNKNOWN;
  if (count > HPA) {
    double hpa = strtod(field[HPA], &end);
    if (end != field[HPA] && hpa >= 0) pos.hpa = (float)hpa;
  }
  return true;
}
