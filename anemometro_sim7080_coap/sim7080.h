// ============================================================
// sim7080.h – Driver AT minimo per SIM7080G: invio con il client
// CoAP del modem.
// Riferimento: SIM7070_SIM7080_SIM7090 Series AT Command Manual
// V1.05, capitolo 18
// ============================================================
#pragma once

#include <Arduino.h>

class Sim7080 {
public:
  Sim7080(Stream& at, uint8_t pwrkeyPin, uint8_t pwrkeyActive);

  void begin();

  // Bloccante (fino a qualche minuto): accende o risveglia il modem se serve,
  // attende la rete, attiva la PDP e prepara il client CoAP.
  // networkMode e' il valore di AT+CMNB: "1" Cat-M, "2" NB-IoT, "3" entrambi.
  bool connect(const char* apn, const char* host, uint16_t port, const char* networkMode = "2");
  bool connected() const { return _connected; }

  // POST su path. Con confirmable=false, true significa "passato al modem";
  // con confirmable=true significa che il server ha risposto.
  bool coapPost(uint16_t messageId, const char* path, uint8_t contentFormat,
                const char* payload, bool confirmable);

  // Sincronizza l'ora del modem via NTP e la legge in secondi Unix. Richiede connect().
  bool syncTime(const char* ntpServer, uint32_t& unixSeconds);

  // Smette di usare il modem, che resta registrato ed entra da solo in PSM
  // quando la rete rilascia la connessione e scade l'active time.
  // Il prossimo connect lo risveglia.
  void standby();

  // Spegnimento completo, solo per ripartire dopo errori ripetuti.
  void powerOff();

private:
  void pulsePwrkey();
  void handleUrc();
  void flushInput();
  void sendCmd(const char* cmd);
  bool lineStartsWith(const char* token) const;
  bool waitLine(const char* ok, const char* fail, uint32_t timeoutMs);
  bool command(const char* cmd, uint32_t timeoutMs = 2000);
  bool query(const char* cmd, const char* expected, uint32_t timeoutMs = 2000);
  bool isAlive();
  bool waitAlive(uint32_t timeoutMs);
  void configurePsm();
  bool configureNetwork(const char* apn, const char* networkMode);
  bool waitSimReady(uint32_t timeoutMs);
  bool waitAttach(uint32_t timeoutMs);
  bool activatePdp(const char* apn);
  bool connectOnce(const char* apn, const char* host, uint16_t port, const char* networkMode);

  Stream& _at;
  uint8_t _pwrkeyPin;
  uint8_t _pwrkeyActive;
  bool    _connected = false;
  bool    _silent = false;  // smesso di rispondere durante la connessione
  char    _line[96];
  size_t  _lineLen   = 0;
};
