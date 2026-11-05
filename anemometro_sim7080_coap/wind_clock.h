// ============================================================
// wind_clock.h – Ora Unix ricavata dalla rete e mantenuta con i tick di FreeRTOS.
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

#define WIND_CLOCK_TICK_HZ 1024  // configTICK_RATE_HZ del BSP

// Conta in tick e non con millis(): su questa scheda millis() vale tick * 1000 / 1024
// troncato a 32 bit, quindi dopo 48,5 giorni salta da 4194303999 a 0 e una differenza a
// cavallo risulta di 28 ore in piu'. I tick invece ripartono da zero in modo regolare.
class WindClock {
public:
  void sync(uint32_t unixSeconds, uint32_t nowTicks) {
    _unixAtSync = unixSeconds;
    _ticksAtSync = nowTicks;
    _synced = true;
  }

  // Una sincronizzazione al giorno basta: il quarzo sbaglia di circa 2 s al giorno
  bool needsSync(uint32_t nowTicks) const {
    return !_synced || nowTicks - _ticksAtSync >= RESYNC_TICKS;
  }

  // Oltre questo limite l'ora non e' piu' affidabile
  bool usable(uint32_t nowTicks) const {
    return _synced && nowTicks - _ticksAtSync < MAX_AGE_TICKS;
  }

  // Ora di un istante vicino alla sincronizzazione, anche precedente: il report si chiude
  // prima che il modem si connetta. Sempre crescente: il server scarta i valori gia' visti.
  uint32_t timestamp(uint32_t ticks) {
    int32_t elapsedTicks = (int32_t)(ticks - _ticksAtSync);
    uint32_t value = _unixAtSync + elapsedTicks / WIND_CLOCK_TICK_HZ;
    if (value <= _last) value = _last + 1;
    _last = value;
    return value;
  }

private:
  static const uint32_t RESYNC_TICKS  = 24UL * 3600UL * WIND_CLOCK_TICK_HZ;
  static const uint32_t MAX_AGE_TICKS = 7UL * 24UL * 3600UL * WIND_CLOCK_TICK_HZ;

  uint32_t _unixAtSync  = 0;
  uint32_t _ticksAtSync = 0;
  uint32_t _last        = 0;
  bool     _synced      = false;
};
