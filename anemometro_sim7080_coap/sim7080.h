// ============================================================
// sim7080.h – Driver AT minimo per SIM7080G: invio datagrammi UDP
// Riferimento: SIM7070_SIM7080_SIM7090 Series TCPUDP(S)
// Application Note V1.03
// ============================================================
#pragma once

#include <Arduino.h>

class Sim7080 {
public:
  // pwrkeyPin < 0 se la scheda accende il modem da sola.
  Sim7080(Stream& at, int pwrkeyPin, uint8_t pwrkeyActive);

  void begin();

  // Bloccante (fino a qualche minuto): accende il modem se serve,
  // attende la rete, attiva la PDP e apre il socket UDP.
  bool connect(const char* apn, const char* host, uint16_t port);
  bool connected() const { return _connected; }

  // Una risposta OK significa "passato al modem", non "ricevuto dal server".
  bool send(const uint8_t* data, size_t len);

  // Interroga la PDP: UDP non segnala da solo la perdita della rete.
  bool checkLink();

  // Impulso su PWRKEY: accende il modem se e' spento, altrimenti lo spegne.
  void togglePower();

private:
  void flushInput();
  void sendCmd(const char* cmd);
  bool waitLine(const char* ok, const char* fail, uint32_t timeoutMs);
  bool waitPrompt(uint32_t timeoutMs);
  bool command(const char* cmd, uint32_t timeoutMs = 2000);
  bool query(const char* cmd, const char* expected, uint32_t timeoutMs = 2000);
  bool isAlive();
  bool waitAlive(uint32_t timeoutMs);
  bool configureApn(const char* apn);
  bool waitSimReady(uint32_t timeoutMs);
  bool waitAttach(uint32_t timeoutMs);
  bool activatePdp(const char* apn);

  Stream& _at;
  int     _pwrkeyPin;
  uint8_t _pwrkeyActive;
  bool    _connected  = false;
  bool    _configured = false;
  char    _line[96];
};
