// ============================================================
// wind_clock.h – Ora Unix ricavata dalla rete e mantenuta con millis().
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stdint.h>
#include <stdio.h>

// Legge "yy/MM/dd,hh:mm:ss+zz" di AT+CCLK, con zz in quarti d'ora dall'ora di Greenwich.
// Ritorna false su una data impossibile o precedente al 2024, come quella di default di
// un modem non sincronizzato. Una data futura ma valida la scarta il server.
inline bool parseModemClock(const char* text, uint32_t& unixSeconds) {
  int yy, month, day, hour, minute, second, zone;
  char sign, closing;
  if (sscanf(text, "\"%2d/%2d/%2d,%2d:%2d:%2d%c%2d%c",
             &yy, &month, &day, &hour, &minute, &second, &sign, &zone, &closing) != 9) {
    return false;
  }
  if (closing != '"' || (sign != '+' && sign != '-') || zone > 96) return false;
  if (yy < 24 || month < 1 || month > 12 || day < 1) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  const int year = 2000 + yy;
  const bool leap = year % 4 == 0;  // basta fino al 2099, l'ultimo anno a due cifre
  static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (day > monthDays[month - 1] + ((month == 2 && leap) ? 1 : 0)) return false;

  // Giorni dal 2000-01-01, che dista 10957 giorni dal 1970
  uint32_t days = 10957 + (uint32_t)yy * 365 + (uint32_t)(yy + 3) / 4;
  for (int m = 1; m < month; m++) days += monthDays[m - 1] + ((m == 2 && leap) ? 1 : 0);
  days += day - 1;

  int32_t offset = (int32_t)zone * 15 * 60;
  if (sign == '-') offset = -offset;
  unixSeconds = days * 86400UL + hour * 3600UL + minute * 60UL + second - offset;
  return true;
}

class WindClock {
public:
  void sync(uint32_t unixSeconds, uint32_t nowMs) {
    _unixAtSync = unixSeconds;
    _msAtSync = nowMs;
    _synced = true;
  }

  // Una sincronizzazione al giorno basta: il quarzo sbaglia di circa 2 s al giorno
  bool needsSync(uint32_t nowMs) const {
    return !_synced || nowMs - _msAtSync >= RESYNC_MS;
  }

  // Oltre questo limite l'ora non e' piu' affidabile, e nemmeno il calcolo di timestamp()
  bool usable(uint32_t nowMs) const {
    return _synced && nowMs - _msAtSync < MAX_AGE_MS;
  }

  // Ora di un istante vicino alla sincronizzazione, anche precedente: il report si chiude
  // prima che il modem si connetta. Sempre crescente: il server scarta i valori gia' visti.
  uint32_t timestamp(uint32_t ms) {
    int32_t elapsedMs = (int32_t)(ms - _msAtSync);
    uint32_t value = _unixAtSync + elapsedMs / 1000;
    if (value <= _last) value = _last + 1;
    _last = value;
    return value;
  }

private:
  static const uint32_t RESYNC_MS  = 24UL * 3600UL * 1000UL;
  static const uint32_t MAX_AGE_MS = 7UL * 24UL * 3600UL * 1000UL;

  uint32_t _unixAtSync = 0;
  uint32_t _msAtSync   = 0;
  uint32_t _last       = 0;
  bool     _synced     = false;
};
