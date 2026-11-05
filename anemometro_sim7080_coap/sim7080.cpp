#include "sim7080.h"

// Durata dell'impulso e attesa da verificare sul Hardware Design del modulo
#define PWRKEY_PULSE_MS    1200UL
#define BOOT_TIMEOUT_MS    20000UL
#define SIM_TIMEOUT_MS     10000UL
#define ATTACH_TIMEOUT_MS  180000UL  // il primo attach Cat-M/NB-IoT e' lento
#define PDP_TIMEOUT_MS     30000UL
#define OPEN_TIMEOUT_MS    30000UL
#define CFUN_TIMEOUT_MS    10000UL
#define PROMPT_TIMEOUT_MS  5000UL
#define SEND_TIMEOUT_MS    10000UL
#define POLL_MS            5

Sim7080::Sim7080(Stream& at, int pwrkeyPin, uint8_t pwrkeyActive)
  : _at(at), _pwrkeyPin(pwrkeyPin), _pwrkeyActive(pwrkeyActive) {}

void Sim7080::begin() {
  // Con il transistor la linea flottante potrebbe premere PWRKEY
  if (_pwrkeyPin >= 0 && _pwrkeyActive != LOW) {
    pinMode(_pwrkeyPin, OUTPUT);
    digitalWrite(_pwrkeyPin, LOW);
  }
}

// ── Primitive AT ─────────────────────────────────────────────
void Sim7080::flushInput() {
  while (_at.available()) _at.read();
}

void Sim7080::sendCmd(const char* cmd) {
  flushInput();  // scarta URC e risposte tardive del comando precedente
  _at.print(cmd);
  _at.print("\r");
}

// Legge righe fino a una che inizia con ok (true) oppure con fail o
// che contiene ERROR (false). Le altre righe, URC compresi, sono ignorate.
bool Sim7080::waitLine(const char* ok, const char* fail, uint32_t timeoutMs) {
  size_t idx = 0;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!_at.available()) {
      delay(POLL_MS);  // cede la CPU al task di campionamento
      continue;
    }
    char c = _at.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (idx < sizeof(_line) - 1) _line[idx++] = c;
      continue;
    }
    _line[idx] = '\0';
    idx = 0;
    if (strncmp(_line, ok, strlen(ok)) == 0) return true;
    if (fail && strncmp(_line, fail, strlen(fail)) == 0) return false;
    if (strstr(_line, "ERROR")) return false;
  }
  return false;
}

// Il prompt '>' non e' seguito da un a capo, quindi non passa da waitLine
bool Sim7080::waitPrompt(uint32_t timeoutMs) {
  size_t idx = 0;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!_at.available()) {
      delay(POLL_MS);
      continue;
    }
    char c = _at.read();
    if (c == '>') return true;
    if (c == '\n') {
      _line[idx] = '\0';
      idx = 0;
      if (strstr(_line, "ERROR")) return false;
    } else if (c != '\r' && idx < sizeof(_line) - 1) {
      _line[idx++] = c;
    }
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

// ── Accensione ───────────────────────────────────────────────
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

void Sim7080::togglePower() {
  _connected  = false;
  _configured = false;
  if (_pwrkeyPin < 0) return;

  Serial.println("SIM7080: impulso PWRKEY");
  pinMode(_pwrkeyPin, OUTPUT);
  digitalWrite(_pwrkeyPin, _pwrkeyActive);
  delay(PWRKEY_PULSE_MS);
  // Attivo basso: a riposo la linea resta flottante, mai forzata a 3.3 V
  if (_pwrkeyActive == LOW) pinMode(_pwrkeyPin, INPUT);
  else                      digitalWrite(_pwrkeyPin, LOW);
}

// ── Rete ─────────────────────────────────────────────────────
bool Sim7080::configureApn(const char* apn) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
  // Alcuni operatori richiedono l'APN gia' in fase di registrazione
  return command("AT+CFUN=0", CFUN_TIMEOUT_MS)
      && command(cmd)
      && command("AT+CFUN=1", CFUN_TIMEOUT_MS);
}

bool Sim7080::waitSimReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    sendCmd("AT+CPIN?");
    // "+CPIN: NOT READY" contiene anch'esso READY
    if (waitLine("+CPIN", "OK", 2000)
        && strstr(_line, "READY") && !strstr(_line, "NOT")) return true;
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
    togglePower();
    if (!waitAlive(BOOT_TIMEOUT_MS)) {
      Serial.println("SIM7080: nessuna risposta AT");
      return false;
    }
  }
  command("ATE0");

  if (!_configured) {
    if (!configureApn(apn)) {
      Serial.println("SIM7080: configurazione APN fallita");
      return false;
    }
    _configured = true;
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

  command("AT+CACLOSE=0");  // puo' dare ERROR se il socket non era aperto

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CAOPEN=0,0,\"UDP\",\"%s\",%u", host, port);
  sendCmd(cmd);
  if (!waitLine("+CAOPEN: 0,0", "+CAOPEN:", OPEN_TIMEOUT_MS)) {
    Serial.println("SIM7080: apertura socket fallita");
    return false;
  }

  Serial.println("SIM7080: connesso");
  _connected = true;
  return true;
}

bool Sim7080::send(const uint8_t* data, size_t len) {
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CASEND=0,%u", (unsigned)len);
  sendCmd(cmd);

  if (!waitPrompt(PROMPT_TIMEOUT_MS)) {
    _connected = false;
    return false;
  }
  _at.write(data, len);
  if (!waitLine("OK", nullptr, SEND_TIMEOUT_MS)) {
    _connected = false;
    return false;
  }
  return true;
}

bool Sim7080::checkLink() {
  if (!query("AT+CNACT?", "+CNACT: 0,1")) _connected = false;
  return _connected;
}
