#include "sim7080.h"

// Durata dell'impulso e attesa da verificare sul Hardware Design del modulo
#define PWRKEY_PULSE_MS    1200UL
#define BOOT_TIMEOUT_MS    20000UL
#define POWER_OFF_MS       10000UL
#define SIM_TIMEOUT_MS     10000UL
#define ATTACH_TIMEOUT_MS  180000UL  // il primo attach Cat-M/NB-IoT e' lento
#define PDP_TIMEOUT_MS     30000UL
#define CFUN_TIMEOUT_MS    10000UL   // massimo indicato dal manuale
#define ACTION_TIMEOUT_MS  10000UL
#define COAP_ACK_MS        100000UL  // comprende le ritrasmissioni CON del modem
#define POLL_MS            5

#define COAP_CODE_POST     2

Sim7080::Sim7080(Stream& at, uint8_t pwrkeyPin, uint8_t pwrkeyActive)
  : _at(at), _pwrkeyPin(pwrkeyPin), _pwrkeyActive(pwrkeyActive) {}

void Sim7080::begin() {
  // Con il transistor la linea flottante potrebbe premere PWRKEY
  if (_pwrkeyActive != LOW) {
    pinMode(_pwrkeyPin, OUTPUT);
    digitalWrite(_pwrkeyPin, LOW);
  }
}

// ── Primitive AT ─────────────────────────────────────────────
// Le righe sono salvate senza spazi: il manuale riporta sia "+CPIN: READY"
// sia "+CPIN:READY", e allo stesso modo per le altre risposte.
bool Sim7080::lineStartsWith(const char* token) const {
  const char* l = _line;
  for (; *token; token++) {
    if (*token == ' ') continue;
    if (*l++ != *token) return false;
  }
  return true;
}

void Sim7080::handleUrc() {
  if (lineStartsWith("+APP PDP: 0,DEACTIVE") || lineStartsWith("+CCOAPACTION: 3")) {
    _connected = false;
  }
}

void Sim7080::flushInput() {
  _lineLen = 0;
  while (_at.available()) {
    char c = _at.read();
    if (c == '\n') {
      _line[_lineLen] = '\0';
      _lineLen = 0;
      handleUrc();
    } else if (c != '\r' && c != ' ' && _lineLen < sizeof(_line) - 1) {
      _line[_lineLen++] = c;
    }
  }
  _lineLen = 0;
}

void Sim7080::sendCmd(const char* cmd) {
  flushInput();  // scarta le risposte tardive del comando precedente
  _at.print(cmd);
  _at.print("\r");
}

// Legge righe fino a una che inizia con ok (true) oppure con fail o
// che contiene ERROR (false). Le altre righe, URC compresi, sono ignorate.
bool Sim7080::waitLine(const char* ok, const char* fail, uint32_t timeoutMs) {
  _lineLen = 0;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!_at.available()) {
      delay(POLL_MS);  // cede la CPU al task di campionamento
      continue;
    }
    char c = _at.read();
    if (c == '\r' || c == ' ') continue;
    if (c != '\n') {
      if (_lineLen < sizeof(_line) - 1) _line[_lineLen++] = c;
      continue;
    }
    _line[_lineLen] = '\0';
    _lineLen = 0;
    if (_line[0] == '\0') continue;
    if (lineStartsWith(ok)) return true;
    if (fail && lineStartsWith(fail)) return false;
    if (strstr(_line, "ERROR")) return false;
    handleUrc();
  }
  return false;
}

bool Sim7080::command(const char* cmd, uint32_t timeoutMs) {
  sendCmd(cmd);
  return waitLine("OK", nullptr, timeoutMs);
}

// true se la risposta contiene una riga che inizia con expected, prima di OK
bool Sim7080::query(const char* cmd, const char* expected, uint32_t timeoutMs) {
  sendCmd(cmd);
  return waitLine(expected, "OK", timeoutMs);
}

// ── Accensione e spegnimento ─────────────────────────────────
bool Sim7080::isAlive() {
  return command("AT", 500);
}

bool Sim7080::waitAlive(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (isAlive()) return true;
    delay(500);
  }
  return false;
}

void Sim7080::pulsePwrkey() {
  Serial.println("SIM7080: impulso PWRKEY");
  pinMode(_pwrkeyPin, OUTPUT);
  digitalWrite(_pwrkeyPin, _pwrkeyActive);
  delay(PWRKEY_PULSE_MS);
  // Attivo basso: a riposo la linea resta flottante, mai forzata a 3.3 V
  if (_pwrkeyActive == LOW) pinMode(_pwrkeyPin, INPUT);
  else                      digitalWrite(_pwrkeyPin, LOW);
}

void Sim7080::powerOff() {
  sendCmd("AT+CPOWD=1");
  // Se non risponde e' gia' spento oppure bloccato: un impulso potrebbe
  // accenderlo, quindi si insiste solo se risponde ancora ai comandi.
  if (!waitLine("NORMAL POWER DOWN", nullptr, POWER_OFF_MS) && isAlive()) {
    pulsePwrkey();
  }
  _connected = false;
  Serial.println("SIM7080: spento");
}

// ── Rete ─────────────────────────────────────────────────────
bool Sim7080::configureApn(const char* apn) {
  char cmd[96];
  // L'APN resta salvato nel modem: riscriverlo a ogni accensione
  // costringerebbe a ripetere CFUN e allungherebbe la registrazione.
  snprintf(cmd, sizeof(cmd), "+CGDCONT: 1,\"IP\",\"%s\"", apn);
  if (query("AT+CGDCONT?", cmd)) return true;

  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
  // Alcuni operatori richiedono l'APN gia' in fase di registrazione
  return command("AT+CFUN=0", CFUN_TIMEOUT_MS)
      && command(cmd)
      && command("AT+CFUN=1", CFUN_TIMEOUT_MS);
}

bool Sim7080::waitSimReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (query("AT+CPIN?", "+CPIN: READY")) return true;
    delay(500);
  }
  return false;
}

bool Sim7080::waitAttach(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (query("AT+CGATT?", "+CGATT: 1")) return true;
    delay(2000);
  }
  return false;
}

bool Sim7080::activatePdp(const char* apn) {
  if (query("AT+CNACT?", "+CNACT: 0,1")) return true;

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CNCFG=0,1,\"%s\"", apn);
  if (!command(cmd)) return false;

  sendCmd("AT+CNACT=0,1");
  return waitLine("+APP PDP: 0,ACTIVE", "+APP PDP: 0,DEACTIVE", PDP_TIMEOUT_MS);
}

bool Sim7080::connect(const char* apn, const char* host, uint16_t port) {
  _connected = false;

  if (!isAlive()) {
    pulsePwrkey();
    if (!waitAlive(BOOT_TIMEOUT_MS)) {
      Serial.println("SIM7080: nessuna risposta AT");
      return false;
    }
  }
  command("ATE0");

  if (!configureApn(apn)) {
    Serial.println("SIM7080: configurazione APN fallita");
    return false;
  }
  if (!waitSimReady(SIM_TIMEOUT_MS)) {
    Serial.println("SIM7080: SIM non pronta");
    return false;
  }
  if (!waitAttach(ATTACH_TIMEOUT_MS)) {
    Serial.println("SIM7080: rete non disponibile");
    return false;
  }
  if (!activatePdp(apn)) {
    Serial.println("SIM7080: attivazione PDP fallita");
    return false;
  }

  command("AT+CCOAPTERM");  // da' ERROR se l'oggetto CoAP non esiste ancora

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CCOAPURL=\"coap://%s:%u\"", host, port);
  if (!command("AT+CCOAPINIT") || !command(cmd)) {
    Serial.println("SIM7080: inizializzazione CoAP fallita");
    return false;
  }

  Serial.println("SIM7080: connesso");
  _connected = true;
  return true;
}

// ── CoAP ─────────────────────────────────────────────────────
bool Sim7080::coapPost(uint16_t messageId, const char* path, uint8_t contentFormat,
                       const char* payload, bool confirmable) {
  flushInput();  // un URC arrivato tra due invii puo' segnalare la rete persa
  if (!_connected) return false;

  char cmd[320];
  int n = snprintf(cmd, sizeof(cmd),
                   "AT+CCOAPPARA=\"CODE\",%d,\"TYPE\",\"%s\",\"MID\",%u,"
                   "\"URI-PATH\",0,\"%s\",\"CONTENT-FORMAT\",%u,\"PAYLOAD\",1,\"",
                   COAP_CODE_POST, confirmable ? "CON" : "NON", messageId,
                   path, contentFormat);

  // Il payload e' passato in esadecimale: le virgolette del JSON
  // chiuderebbero la stringa del comando AT.
  size_t payloadLen = strlen(payload);
  if (n < 0 || (size_t)n + payloadLen * 2 + 2 > sizeof(cmd)) return false;
  for (size_t i = 0; i < payloadLen; i++) {
    n += snprintf(cmd + n, sizeof(cmd) - n, "%02x", (uint8_t)payload[i]);
  }
  cmd[n++] = '"';
  cmd[n]   = '\0';

  bool sent = command(cmd);
  if (sent) {
    sendCmd("AT+CCOAPACTION");
    sent = waitLine("+CCOAPACTION: 0,", "+CCOAPACTION:", ACTION_TIMEOUT_MS);
  }
  if (sent && confirmable) {
    sent = waitLine("+CCOAPRECV:", "+CCOAPACTION:", COAP_ACK_MS);
    command("AT+CCOAPACTION=5");  // la risposta non serve: libera la coda
  }

  if (!sent) _connected = false;
  return sent;
}

bool Sim7080::checkLink() {
  command("AT+CCOAPACTION=5");  // evita che risposte non lette riempiano la coda
  if (!query("AT+CNACT?", "+CNACT: 0,1")) _connected = false;
  return _connected;
}
