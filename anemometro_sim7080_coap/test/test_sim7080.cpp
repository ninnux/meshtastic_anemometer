// Test sul computer del driver SIM7080 con un modem simulato e della gestione accensione.
// Dalla cartella anemometro_sim7080_coap:
//   g++ -std=c++17 -Itest -I. test/test_sim7080.cpp sim7080.cpp -o /tmp/test_sim7080 && /tmp/test_sim7080
// Se su macOS non trova gli header C++, aggiungere:
//   -isysroot $(xcrun --show-sdk-path) -I$(xcrun --show-sdk-path)/usr/include/c++/v1
#include <stdlib.h>
#include <vector>
#include "Arduino.h"
#include "modem_power.h"
#include "sim7080.h"

uint32_t fakeMillis = 0;
int pwrkeyPulses = 0;
uint8_t pwrkeyLevel = LOW;
FakeSerial Serial;

class FakeModem;
static FakeModem* activeModem = nullptr;

class FakeModem : public Stream {
public:
  bool powered = true;
  uint32_t bootAt = 0;
  bool simReady = true;
  int attachAfterPolls = 0;
  bool pdpActive = false;
  bool apnStored = false;
  std::string storedPsm;
  bool psmSupported = true;
  bool inPsm = false;
  bool keepsContextInPsm = true;
  bool answersPowerOff = true;
  bool coapOk = true;
  int actionError = 0;
  bool serverAnswers = true;
  std::string urcBeforeReply;
  std::vector<std::string> commands;

  int available() override { return rx.size(); }
  int read() override {
    if (rx.empty()) return -1;
    char c = rx[0];
    rx.erase(0, 1);
    return (uint8_t)c;
  }
  size_t write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; i++) onByte(data[i]);
    return len;
  }
  void enterPsm() {
    inPsm = true;
    rx.clear();
    if (!keepsContextInPsm) pdpActive = false;
  }
  void togglePower() {
    if (inPsm) {  // l'impulso risveglia, non spegne
      inPsm = false;
      bootAt = fakeMillis + 1000;
      return;
    }
    powered = !powered;
    pdpActive = false;
    rx.clear();
    if (powered) bootAt = fakeMillis + 3000;
  }
  void injectUrc(const std::string& urc) { rx += urc; }
  int count(const std::string& prefix) const {
    int n = 0;
    for (auto& c : commands) if (c.rfind(prefix, 0) == 0) n++;
    return n;
  }
  std::string last(const std::string& prefix) const {
    for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
      if (it->rfind(prefix, 0) == 0) return *it;
    }
    return "";
  }

private:
  std::string rx, cmd;

  void onByte(uint8_t b) {
    if (b != '\r') { cmd += (char)b; return; }
    std::string c = cmd;
    cmd.clear();
    if (!powered || inPsm || fakeMillis < bootAt) return;
    commands.push_back(c);
    rx += urcBeforeReply;
    reply(c);
  }

  void reply(const std::string& c) {
    if (c == "AT+CPIN?") {
      rx += simReady ? "\r\n+CPIN: READY\r\n\r\nOK\r\n" : "\r\n+CPIN: NOT READY\r\n\r\nOK\r\n";
    } else if (c == "AT+CGATT?") {
      rx += attachAfterPolls-- <= 0 ? "\r\n+CGATT: 1\r\n\r\nOK\r\n" : "\r\n+CGATT: 0\r\n\r\nOK\r\n";
    } else if (c == "AT+CGDCONT?") {
      if (apnStored) rx += "\r\n+CGDCONT: 1,\"IP\",\"internet\",\"0.0.0.0\",0,0,0,0\r\n";
      rx += "\r\nOK\r\n";
    } else if (c.rfind("AT+CGDCONT=", 0) == 0) {
      apnStored = true;
      rx += "\r\nOK\r\n";
    } else if (c == "AT+CPSMS?") {
      rx += "\r\n+CPSMS: " + (storedPsm.empty() ? std::string("0,,,\"01100000\",\"00000000\"") : storedPsm) + "\r\n\r\nOK\r\n";
    } else if (c.rfind("AT+CPSMS=", 0) == 0) {
      if (psmSupported) {
        storedPsm = c.substr(strlen("AT+CPSMS="));
        rx += "\r\nOK\r\n";
      } else {
        rx += "\r\nERROR\r\n";
      }
    } else if (c == "AT+CPSMRDP") {
      rx += "\r\n+CPSMRDP: 1,30,7200,30,7200,0\r\n\r\nOK\r\n";
    } else if (c == "AT+CNACT?") {
      // Il manuale riporta questa risposta sia con sia senza spazio
      rx += pdpActive ? "\r\n+CNACT:0,1,\"10.0.0.2\"\r\n" : "\r\n+CNACT:0,0,\"0.0.0.0\"\r\n";
      rx += "+CNACT: 1,0,\"0.0.0.0\"\r\n\r\nOK\r\n";
    } else if (c == "AT+CNACT=0,1") {
      pdpActive = true;
      rx += "\r\nOK\r\n\r\n+APP PDP: 0,ACTIVE\r\n";
    } else if (c == "AT+CPOWD=1") {
      if (!answersPowerOff) return;
      powered = false;
      pdpActive = false;
      rx += "\r\nNORMAL POWER DOWN\r\n";
    } else if (c.rfind("AT+CCOAPPARA=", 0) == 0) {
      rx += coapOk ? "\r\nOK\r\n" : "\r\nERROR\r\n";
    } else if (c == "AT+CCOAPACTION") {
      if (actionError) {
        rx += "\r\n+CCOAPACTION: " + std::to_string(actionError) + ",7\r\n\r\nOK\r\n";
      } else {
        rx += "\r\n+CCOAPACTION: 0,7\r\n\r\nOK\r\n";
        if (serverAnswers && last("AT+CCOAPPARA").find("\"CON\"") != std::string::npos) {
          rx += "\r\n+CCOAPRECV: 7,6,0\r\n";
        }
      }
    } else {
      rx += "\r\nOK\r\n";
    }
  }
};

void delay(uint32_t ms) { fakeMillis += ms; }

void digitalWrite(int, int level) {
  if (level == HIGH && pwrkeyLevel == LOW) {
    pwrkeyPulses++;
    if (activeModem) activeModem->togglePower();
  }
  pwrkeyLevel = level;
}

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); failures++; } } while (0)

static const char* APN = "internet";
static const char* HOST = "192.0.2.1";

static void reset(FakeModem& m) {
  fakeMillis = 0;
  pwrkeyPulses = 0;
  pwrkeyLevel = LOW;
  activeModem = &m;
}

static const char* PAYLOAD = "{\"id\":\"a\",\"dir\":225}";
static const char* PAYLOAD_HEX = "7b226964223a2261222c22646972223a3232357d";

static void testColdBootConnectAndPost() {
  FakeModem m; reset(m);
  m.powered = false;
  m.attachAfterPolls = 2;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.connected());
  CHECK(pwrkeyPulses == 1);
  CHECK(m.count("AT+CGDCONT=1,\"IP\",\"internet\"") == 1);
  CHECK(m.count("AT+CNCFG=0,1,\"internet\"") == 1);
  CHECK(m.count("AT+CCOAPINIT") == 1);
  CHECK(m.count("AT+CCOAPURL=\"coap://192.0.2.1:5683\"") == 1);
  CHECK(m.count("AT+CGATT?") == 3);

  CHECK(modem.coapPost(513, "wind", 50, PAYLOAD, false));
  std::string expected = std::string("AT+CCOAPPARA=\"CODE\",2,\"TYPE\",\"NON\",\"MID\",513,"
                                     "\"URI-PATH\",0,\"wind\",\"CONTENT-FORMAT\",50,\"PAYLOAD\",1,\"")
                         + PAYLOAD_HEX + "\"";
  CHECK(m.last("AT+CCOAPPARA") == expected);
  CHECK(m.count("AT+CCOAPACTION") == 1);
  CHECK(modem.connected());
}

// Un impulso a modem acceso lo spegnerebbe
static void testRunningModemNotPulsed() {
  FakeModem m; reset(m);
  m.pdpActive = true;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(pwrkeyPulses == 0);
  CHECK(m.count("AT+CNACT=0,1") == 0);
}

// Con l'APN gia' salvato si evita CFUN, che allungherebbe ogni riaccensione
static void testStoredApnSkipsCfun() {
  FakeModem m; reset(m);
  m.apnStored = true;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CFUN") == 0);
  CHECK(m.count("AT+CGDCONT=") == 0);
}

static void testModemDead() {
  FakeModem m; reset(m);
  m.powered = false;
  m.bootAt = 0xFFFFFFFF;
  activeModem = nullptr;  // l'impulso non ha effetto
  Sim7080 modem(m, 1, HIGH);
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(!modem.connected());
  CHECK(fakeMillis < 60000);
}

static void testSimNotReady() {
  FakeModem m; reset(m);
  m.simReady = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CGATT?") == 0);
}

static void testNoNetworkTimesOut() {
  FakeModem m; reset(m);
  m.attachAfterPolls = 1000000;
  Sim7080 modem(m, 1, HIGH);
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(fakeMillis >= 180000 && fakeMillis < 200000);
}

static void testPostParaErrorMarksDisconnected() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  m.coapOk = false;
  CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, false));
  CHECK(!modem.connected());
  CHECK(m.count("AT+CCOAPACTION") == 0);
}

static void testActionErrorMarksDisconnected() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  m.actionError = 3;
  CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, false));
  CHECK(!modem.connected());
}

static void testOversizedPayloadRejected() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  std::string big(200, 'x');
  CHECK(!modem.coapPost(1, "wind", 50, big.c_str(), false));
  CHECK(m.count("AT+CCOAPPARA") == 0);
}

static void testConfirmableAcknowledged() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, true));
  CHECK(m.last("AT+CCOAPPARA").find("\"TYPE\",\"CON\"") != std::string::npos);
  CHECK(m.count("AT+CCOAPACTION=5") == 1);
  CHECK(modem.connected());
}

static void testConfirmableServerSilent() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  m.serverAnswers = false;
  uint32_t start = fakeMillis;
  CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, true));
  CHECK(!modem.connected());
  CHECK(fakeMillis - start < 120000);
}

static void testUrcsAreIgnored() {
  FakeModem m; reset(m);
  m.urcBeforeReply = "\r\n+CCOAPRECV: 1,14,9\r\n";
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
  CHECK(modem.connected());
}

// Un URC arrivato tra due invii non deve andare perso con lo svuotamento del buffer
static void testUrcBetweenPostsDetected() {
  const char* urcs[] = {"\r\n+APP PDP: 0,DEACTIVE\r\n", "\r\n+CCOAPACTION: 3\r\n"};
  for (const char* urc : urcs) {
    FakeModem m; reset(m);
    Sim7080 modem(m, 1, HIGH);
    CHECK(modem.connect(APN, HOST, 5683));
    m.injectUrc(urc);
    CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, false));
    CHECK(!modem.connected());
    CHECK(m.count("AT+CCOAPPARA") == 0);
  }
}

static void testCheckLink() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.checkLink());
  m.pdpActive = false;
  CHECK(!modem.checkLink());
  CHECK(!modem.connected());
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CNACT=0,1") == 2);
}

static void testPowerOffThenOnAgain() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.powerOff();
  CHECK(!m.powered);
  CHECK(!modem.connected());
  CHECK(pwrkeyPulses == 0);  // spento con AT+CPOWD, senza impulso

  CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, false));

  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.powered);
  CHECK(pwrkeyPulses == 1);
  CHECK(m.count("AT+CFUN") == 2);  // CFUN=0 e CFUN=1 solo alla prima accensione
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
}

static void testPowerOffFallsBackToPwrkey() {
  FakeModem m; reset(m);
  m.answersPowerOff = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.powerOff();
  CHECK(pwrkeyPulses == 1);
  CHECK(!m.powered);
}

// Se e' gia' spento un impulso lo accenderebbe
static void testPowerOffWhenAlreadyOffNotPulsed() {
  FakeModem m; reset(m);
  m.powered = false;
  Sim7080 modem(m, 1, HIGH);
  modem.powerOff();
  CHECK(pwrkeyPulses == 0);
  CHECK(!m.powered);
}

static const char* PSM_CMD = "AT+CPSMS=1,,,\"00100010\",\"00001111\"";

static void testPsmRequestedOnce() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count(PSM_CMD) == 1);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CPSMS=") == 1);  // gia' salvato: riscriverlo forzerebbe un nuovo TAU
}

static void testPsmRefusedStillConnects() {
  FakeModem m; reset(m);
  m.psmSupported = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
}

static void testStandbyDoesNotPowerOff() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.standby();
  CHECK(!modem.connected());
  CHECK(m.powered);
  CHECK(m.count("AT+CPOWD") == 0);
  CHECK(pwrkeyPulses == 0);
}

static void testWakeFromPsmKeepsContext() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.standby();
  m.enterPsm();
  m.commands.clear();

  uint32_t start = fakeMillis;
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(!m.inPsm);
  CHECK(m.powered);
  CHECK(pwrkeyPulses == 1);
  CHECK(fakeMillis - start < 5000);
  CHECK(m.count("AT+CFUN") == 0);
  CHECK(m.count("AT+CPSMS=") == 0);
  CHECK(m.count("AT+CNACT=0,1") == 0);
  CHECK(m.count("AT+CCOAPINIT") == 1);
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
}

// Il manuale non dice se la PDP sopravvive al PSM: va gestito anche il caso peggiore
static void testWakeFromPsmLosesContext() {
  FakeModem m; reset(m);
  m.keepsContextInPsm = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.standby();
  m.enterPsm();
  m.commands.clear();

  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CNACT=0,1") == 1);
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
}

// Risvegliato da solo per il TAU, risponde: un impulso lo spegnerebbe
static void testStandbyNotYetInPsmNotPulsed() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  modem.standby();
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(pwrkeyPulses == 0);
  CHECK(m.powered);
}

// Il modem si addormenta mentre lo si crede attivo: l'invio fallisce e si riprende
static void testPsmWhileConnectedRecovers() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  m.enterPsm();
  CHECK(!modem.coapPost(1, "wind", 50, PAYLOAD, false));
  CHECK(!modem.connected());
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(2, "wind", 50, PAYLOAD, false));
  CHECK(pwrkeyPulses == 1);
}

static void testPolicy() {
  const uint32_t MIN = 60000;
  ModemPowerPolicy p(3.0f, 10 * MIN, 30 * MIN);
  uint32_t t = 1000;
  p.begin(t);
  CHECK(p.modemNeeded(t));

  // Vento: resta acceso. Calma: si spegne dopo 10 minuti esatti
  p.onReport(8.0f, t);
  p.onReport(2.9f, t + 5 * MIN);
  CHECK(p.modemNeeded(t + 10 * MIN - 1));
  p.onHeartbeatDone(t + 9 * MIN);
  CHECK(!p.modemNeeded(t + 10 * MIN));

  // Il messaggio di presenza arriva 30 minuti dopo l'ultimo invio
  CHECK(!p.modemNeeded(t + 39 * MIN - 1));
  CHECK(p.modemNeeded(t + 39 * MIN));
  p.onHeartbeatDone(t + 39 * MIN);
  CHECK(!p.modemNeeded(t + 39 * MIN));

  // Una raffica riaccende subito, con soglia inclusa
  p.onReport(3.0f, t + 40 * MIN);
  CHECK(p.modemNeeded(t + 40 * MIN));
}

static void testPolicyMillisWrap() {
  ModemPowerPolicy p(3.0f, 1000, 5000);
  uint32_t t = 0xFFFFFF00;
  p.begin(t);
  CHECK(p.modemNeeded(t + 500));
  CHECK(!p.modemNeeded(t + 1500));  // t + 1500 ha gia' superato lo zero
  CHECK(p.modemNeeded(t + 5000));
}

int main() {
  testColdBootConnectAndPost();
  testRunningModemNotPulsed();
  testStoredApnSkipsCfun();
  testModemDead();
  testSimNotReady();
  testNoNetworkTimesOut();
  testPostParaErrorMarksDisconnected();
  testActionErrorMarksDisconnected();
  testOversizedPayloadRejected();
  testConfirmableAcknowledged();
  testConfirmableServerSilent();
  testUrcsAreIgnored();
  testUrcBetweenPostsDetected();
  testCheckLink();
  testPowerOffThenOnAgain();
  testPowerOffFallsBackToPwrkey();
  testPowerOffWhenAlreadyOffNotPulsed();
  testPsmRequestedOnce();
  testPsmRefusedStillConnects();
  testStandbyDoesNotPowerOff();
  testWakeFromPsmKeepsContext();
  testWakeFromPsmLosesContext();
  testStandbyNotYetInPsmNotPulsed();
  testPsmWhileConnectedRecovers();
  testPolicy();
  testPolicyMillisWrap();
  printf(failures ? "%d test falliti\n" : "tutti i test superati\n", failures);
  return failures ? 1 : 0;
}
