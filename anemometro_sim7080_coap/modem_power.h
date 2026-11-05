// ============================================================
// modem_power.h – Decide quando il modem deve restare acceso.
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stdint.h>

// Il modem serve se c'e' stato vento di recente o se e' ora del messaggio
// di presenza, che distingue una stazione nella calma da una guasta.
class ModemPowerPolicy {
public:
  ModemPowerPolicy(float activeKn, uint32_t calmOffMs, uint32_t heartbeatMs)
    : _activeKn(activeKn), _calmOffMs(calmOffMs), _heartbeatMs(heartbeatMs) {}

  void begin(uint32_t now) {
    _lastWind = now;
    _lastHeartbeat = now;
  }

  void onReport(float maxKn, uint32_t now) {
    if (maxKn >= _activeKn) _lastWind = now;
  }

  // Chiamata anche quando il messaggio di presenza non e' riuscito a partire:
  // senza copertura non si deve tenere acceso il modem a riprovare di continuo.
  void onHeartbeatDone(uint32_t now) {
    _lastHeartbeat = now;
  }

  bool modemNeeded(uint32_t now) const {
    return (now - _lastWind) < _calmOffMs
        || (now - _lastHeartbeat) >= _heartbeatMs;
  }

private:
  float    _activeKn;
  uint32_t _calmOffMs;
  uint32_t _heartbeatMs;
  uint32_t _lastWind      = 0;
  uint32_t _lastHeartbeat = 0;
};
