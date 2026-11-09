#include "sim7080.h"
#include "wind_clock.h"
#include "log_serial.h"

// Durata dell'impulso e attesa da verificare sul Hardware Design del modulo
#define PWRKEY_PULSE_MS    1200UL
#define BOOT_TIMEOUT_MS    20000UL
#define POWER_OFF_MS       10000UL
#define SIM_TIMEOUT_MS     10000UL
#define ATTACH_TIMEOUT_MS  180000UL  // il primo attach Cat-M/NB-IoT e' lento
#define PDP_TIMEOUT_MS     30000UL
#define PDP_RESPONSE_MS    10000UL
#define PDP_RETRY_MS       500UL
#define CFUN_TIMEOUT_MS    10000UL   // massimo indicato dal manuale
#define ACTION_TIMEOUT_MS  10000UL
#define NTP_TIMEOUT_MS     30000UL
#define NTP_ATTEMPTS       3
#define NTP_RETRY_MS       3000UL    // appena uscito dal PSM il DNS puo' non essere pronto
#define COAP_ACK_MS        100000UL  // comprende le ritrasmissioni CON del modem
#define POLL_MS            5

#define COAP_CODE_POST     2
#define COAP_MID_LIMIT     10000  // il modem risponde ERROR a CCOAPPARA con MID oltre 9999

// Di fabbrica un modem puo' avere NB-IoT solo sulla banda 8, con cui in Italia
// non trova rete: Vodafone NB-IoT usa la banda 20.
#define NB_IOT_BANDS       "3,8,20"
#define AT_LINE_MAX        559  // limite della riga di comando secondo il manuale

// Valori richiesti alla rete, che puo' assegnarne altri (vedi AT+CPSMRDP).
// Codifica in 3GPP TS 24.008: 3 bit di unita' + 5 bit di valore.
// L'active time e' il minimo diverso da zero, cosi' il modem dorme subito dopo
// l'invio. Per il manuale un valore assegnato pari a 0 indica PSM non supportato.
// Il modem normalizza il valore che riceve e AT+CPSMS? riporta questo: con un'altra
// scrittura della stessa durata verrebbe riscritto in flash a ogni risveglio.
#define PSM_PERIODIC_TAU   "00001100"  // unita' 10 min x 12 = 2 ore
#define PSM_ACTIVE_TIME    "00000001"  // unita' 2 s x 1 = 2 s

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

// Con SIM7080_TRACE ogni comando e ogni riga ricevuta vanno anche sulla USB
#ifdef SIM7080_TRACE
#define TRACE(prefix, text) do { Log.print(prefix); Log.println(text); } while (0)
#else
#define TRACE(prefix, text) do {} while (0)
#endif

void Sim7080::flushInput() {
  _lineLen = 0;
  while (_at.available()) {
    char c = _at.read();
    if (c == '\n') {
      _line[_lineLen] = '\0';
      _lineLen = 0;
      if (_line[0]) TRACE("<< ", _line);
      handleUrc();
    } else if (c != '\r' && c != ' ' && _lineLen < sizeof(_line) - 1) {
      _line[_lineLen++] = c;
    }
  }
  _lineLen = 0;
}

void Sim7080::sendCmd(const char* cmd) {
  flushInput();  // scarta le risposte tardive del comando precedente
  TRACE(">> ", cmd);
  _at.print(cmd);
  _at.print("\r");
}

// Legge righe fino a una che inizia con ok (true) oppure con fail o
// che contiene ERROR (false). Le altre righe, URC compresi, sono ignorate.
bool Sim7080::waitLine(const char* ok, const char* fail, uint32_t timeoutMs) {
  _lineLen = 0;
  uint32_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < ms2tick(timeoutMs)) {
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
    TRACE("<< ", _line);
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
// Piu' tentativi: un impulso a un modem solo lento lo spegnerebbe
bool Sim7080::isAlive() {
  for (uint8_t i = 0; i < 3; i++) {
    if (command("AT", 500)) return true;
  }
  return false;
}

bool Sim7080::waitAlive(uint32_t timeoutMs) {
  uint32_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < ms2tick(timeoutMs)) {
    if (isAlive()) return true;
    delay(500);
  }
  return false;
}

void Sim7080::pulsePwrkey() {
  Log.println("SIM7080: impulso PWRKEY");
  pinMode(_pwrkeyPin, OUTPUT);
  digitalWrite(_pwrkeyPin, _pwrkeyActive);
  delay(PWRKEY_PULSE_MS);
  // Attivo basso: a riposo la linea resta flottante, mai forzata a 3.3 V
  if (_pwrkeyActive == LOW) pinMode(_pwrkeyPin, INPUT);
  else                      digitalWrite(_pwrkeyPin, LOW);
}

void Sim7080::standby() {
  _connected = false;
  Log.println("SIM7080: in standby, entrera' in PSM");
}

void Sim7080::powerOff() {
  sendCmd("AT+CPOWD=1");
  // Se non risponde e' gia' spento oppure bloccato: un impulso potrebbe
  // accenderlo, quindi si insiste solo se risponde ancora ai comandi.
  if (!waitLine("NORMAL POWER DOWN", nullptr, POWER_OFF_MS) && isAlive()) {
    pulsePwrkey();
  }
  _connected = false;
  Log.println("SIM7080: spento");
}

// ── Rete ─────────────────────────────────────────────────────
// Non e' un errore bloccante: senza PSM il modem funziona, consumando di piu'
void Sim7080::configurePsm() {
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "+CPSMS: 1,,,\"%s\",\"%s\"", PSM_PERIODIC_TAU, PSM_ACTIVE_TIME);
  if (query("AT+CPSMS?", cmd)) return;

  snprintf(cmd, sizeof(cmd), "AT+CPSMS=1,,,\"%s\",\"%s\"", PSM_PERIODIC_TAU, PSM_ACTIVE_TIME);
  if (!command(cmd)) Log.println("SIM7080: configurazione PSM fallita");
}

bool Sim7080::configureNetwork(const char* apn, const char* networkMode) {
  char expectedMode[16];
  snprintf(expectedMode, sizeof(expectedMode), "+CMNB: %s", networkMode);
  char expectedApn[96];
  snprintf(expectedApn, sizeof(expectedApn), "+CGDCONT: 1,\"IP\",\"%s\"", apn);

  // Sono salvati nel modem: riscriverli a ogni accensione costringerebbe a ripetere CFUN e
  // allungherebbe la registrazione. Se una lettura non da' OK il modem non e' ancora pronto,
  // e non si puo' dire che i valori siano diversi: si riprova al prossimo avvio.
  const char* const reads[][2] = {
    {"AT+CMNB?", expectedMode},
    {"AT+CBANDCFG?", "+CBANDCFG: \"NB-IOT\"," NB_IOT_BANDS},
    {"AT+CGDCONT?", expectedApn},
  };
  bool different = false;
  for (const auto& read : reads) {
    if (query(read[0], read[1])) continue;
    if (!lineStartsWith("OK")) return true;
    different = true;
  }
  if (!different) return true;

  char writeMode[16];
  snprintf(writeMode, sizeof(writeMode), "AT+CMNB=%s", networkMode);
  char writeApn[96];
  snprintf(writeApn, sizeof(writeApn), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
  // Alcuni operatori richiedono l'APN gia' in fase di registrazione
  return command("AT+CFUN=0", CFUN_TIMEOUT_MS)
      && command(writeMode)
      && command("AT+CBANDCFG=\"NB-IOT\"," NB_IOT_BANDS)
      && command(writeApn)
      && command("AT+CFUN=1", CFUN_TIMEOUT_MS);
}

bool Sim7080::waitSimReady(uint32_t timeoutMs) {
  uint32_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < ms2tick(timeoutMs)) {
    if (query("AT+CPIN?", "+CPIN: READY")) return true;
    delay(500);
  }
  return false;
}

bool Sim7080::waitAttach(uint32_t timeoutMs) {
  uint32_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < ms2tick(timeoutMs)) {
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

  // Dopo il PSM AT+CGATT? riporta 1 anche se la radio non e' ancora pronta: i primi
  // tentativi falliscono con DEACTIVE, per circa 4 s nelle prove con il modem vero.
  uint32_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < ms2tick(PDP_TIMEOUT_MS)) {
    sendCmd("AT+CNACT=0,1");
    if (waitLine("+APP PDP: 0,ACTIVE", "+APP PDP: 0,DEACTIVE", PDP_RESPONSE_MS)) return true;
    delay(PDP_RETRY_MS);
  }
  return false;
}

bool Sim7080::connect(const char* apn, const char* host, uint16_t port, const char* networkMode) {
  _connected = false;

  if (!isAlive()) {
    pulsePwrkey();
    if (!waitAlive(BOOT_TIMEOUT_MS)) {
      Log.println("SIM7080: nessuna risposta AT");
      return false;
    }
  }
  command("ATE0");

  if (!configureNetwork(apn, networkMode)) {
    Log.println("SIM7080: configurazione rete fallita");
    return false;
  }
  if (!waitSimReady(SIM_TIMEOUT_MS)) {
    Log.println("SIM7080: SIM non pronta");
    return false;
  }
  if (!waitAttach(ATTACH_TIMEOUT_MS)) {
    Log.println("SIM7080: rete non disponibile");
    return false;
  }
  // Solo ora: appena sveglio il modem risponde ad AT ma AT+CPSMS? da' ERROR
  configurePsm();
  if (!activatePdp(apn)) {
    Log.println("SIM7080: attivazione PDP fallita");
    return false;
  }

  command("AT+CCOAPTERM");  // da' ERROR se l'oggetto CoAP non esiste ancora

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CCOAPURL=\"coap://%s:%u\"", host, port);
  if (!command("AT+CCOAPINIT") || !command(cmd)) {
    Log.println("SIM7080: inizializzazione CoAP fallita");
    return false;
  }

  // Il quarto valore e' l'active time assegnato dalla rete: 0 = PSM non supportato
  if (query("AT+CPSMRDP", "+CPSMRDP:")) Log.println(_line);

  Log.println("SIM7080: connesso");
  _connected = true;
  return true;
}

// ── Ora ──────────────────────────────────────────────────────
// Il manuale non indica il formato dell'ora restituita da AT+CNTP, mentre quello di
// AT+CCLK e' documentato: si sincronizza il modem con NTP e poi si legge il suo orologio.
bool Sim7080::syncTime(const char* ntpServer, uint32_t& unixSeconds) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CNTP=\"%s\",0,0,0", ntpServer);
  if (!command(cmd)) return false;

  // Nelle prove, al risveglio dal PSM, AT+CNTP risponde a volte 62 (errore DNS)
  // e riesce al ciclo dopo: qualche tentativo evita di perdere il report.
  bool synced = false;
  for (int attempt = 0; attempt < NTP_ATTEMPTS && !synced; attempt++) {
    if (attempt > 0) delay(NTP_RETRY_MS);
    sendCmd("AT+CNTP");
    synced = waitLine("+CNTP: 1", "+CNTP:", NTP_TIMEOUT_MS);
  }
  if (!synced) return false;

  sendCmd("AT+CCLK?");
  if (!waitLine("+CCLK:", "OK", 2000)) return false;
  return parseModemClock(_line + strlen("+CCLK:"), unixSeconds);
}

// ── CoAP ─────────────────────────────────────────────────────
bool Sim7080::coapPost(uint16_t messageId, const char* path, uint8_t contentFormat,
                       const char* payload, bool confirmable) {
  flushInput();  // un URC arrivato tra due invii puo' segnalare la rete persa
  if (!_connected) return false;

  char cmd[AT_LINE_MAX + 1];
  int n = snprintf(cmd, sizeof(cmd),
                   "AT+CCOAPPARA=\"CODE\",%d,\"TYPE\",\"%s\",\"MID\",%u,"
                   "\"URI-PATH\",0,\"%s\",\"CONTENT-FORMAT\",%u,\"PAYLOAD\",1,\"",
                   COAP_CODE_POST, confirmable ? "CON" : "NON", messageId % COAP_MID_LIMIT,
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
    // Indica alla rete che puo' rilasciare subito la connessione, cosi' il PSM
    // arriva prima. Il manuale lo prevede solo per NB-IoT: l'esito non conta.
    command(confirmable ? "AT+CRAI=2,0" : "AT+CRAI=1,0");

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
