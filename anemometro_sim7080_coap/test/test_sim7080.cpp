// Test sul computer del driver SIM7080 con un modem simulato.
// Dalla cartella anemometro_sim7080_coap:
//   g++ -std=c++17 -Itest -I. test/test_sim7080.cpp sim7080.cpp -o /tmp/test_sim7080 && /tmp/test_sim7080
// Se su macOS non trova gli header C++, aggiungere:
//   -isysroot $(xcrun --show-sdk-path) -I$(xcrun --show-sdk-path)/usr/include/c++/v1
#include <algorithm>
#include <stdlib.h>
#include <vector>
#include "Arduino.h"
#include "sim7080.h"
#include "hmac_sha256.h"
#include "wind_clock.h"
#include "wind_report.h"

uint32_t fakeMillis = 0;
uint32_t fakeTickOffset = 0;
int pwrkeyPulses = 0;
uint8_t pwrkeyLevel = LOW;
FakeSerial Serial;
FakeSerial Log;

class FakeModem;
static FakeModem* activeModem = nullptr;

class FakeModem : public Stream {
public:
  bool powered = true;
  uint32_t bootAt = 0;
  bool simReady = true;
  int attachAfterPolls = 0;
  bool attachNeedsRadioRestart = false;  // si registra solo dopo CFUN=0 e CFUN=1
  bool pdpActive = false;
  bool apnStored = false;
  int bootingCommands = 0;  // appena sveglio: AT e' ok, il resto no
  std::string networkMode = "1";
  std::string nbBands = "8";
  int cnactFailures = 0;
  std::string storedPsm;
  bool psmSupported = true;
  bool inPsm = false;
  bool keepsContextInPsm = true;
  bool answersPowerOff = true;
  bool coapOk = true;
  std::string ntpResult = "+CNTP: 1";
  int ntpFailures = 0;  // risposte con errore DNS prima di ntpResult
  std::string clock = "\"26/11/05,11:53:02+00\"";
  bool raiSupported = true;
  int ignoreCommands = 0;
  int cncfgSilent = 0;  // AT+CNCFG senza risposta, come appena uscito dal PSM
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
    if (ignoreCommands > 0) { ignoreCommands--; return; }
    commands.push_back(c);
    if (cncfgSilent > 0 && c.rfind("AT+CNCFG=", 0) == 0) { cncfgSilent--; return; }
    rx += urcBeforeReply;
    if (bootingCommands > 0 && c != "AT" && c != "ATE0") {
      bootingCommands--;
      if (c == "AT+CPIN?") rx += "\r\n+CPIN: NOT READY\r\n\r\nOK\r\n";
      else rx += "\r\nERROR\r\n";
      return;
    }
    reply(c);
  }

  void reply(const std::string& c) {
    if (c == "AT+CPIN?") {
      rx += simReady ? "\r\n+CPIN: READY\r\n\r\nOK\r\n" : "\r\n+CPIN: NOT READY\r\n\r\nOK\r\n";
    } else if (c == "AT+CGATT?") {
      bool attached = attachAfterPolls-- <= 0 && !attachNeedsRadioRestart;
      rx += attached ? "\r\n+CGATT: 1\r\n\r\nOK\r\n" : "\r\n+CGATT: 0\r\n\r\nOK\r\n";
    } else if (c == "AT+CFUN=1") {
      attachNeedsRadioRestart = false;
      rx += "\r\nOK\r\n";
    } else if (c == "AT+CMNB?") {
      rx += "\r\n+CMNB: " + networkMode + "\r\n\r\nOK\r\n";
    } else if (c.rfind("AT+CMNB=", 0) == 0) {
      networkMode = c.substr(strlen("AT+CMNB="));
      rx += "\r\nOK\r\n";
    } else if (c == "AT+CBANDCFG?") {
      rx += "\r\n+CBANDCFG: \"CAT-M\",1,2,3,4,5,8,12,13,14,18,19,20,25,26,27,28,66,85\r\n";
      rx += "+CBANDCFG: \"NB-IOT\"," + nbBands + "\r\n\r\nOK\r\n";
    } else if (c.rfind("AT+CBANDCFG=\"NB-IOT\",", 0) == 0) {
      nbBands = c.substr(strlen("AT+CBANDCFG=\"NB-IOT\","));
      rx += "\r\nOK\r\n";
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
      if (cnactFailures != 0) {  // -1: non si attiva mai
        if (cnactFailures > 0) cnactFailures--;
        rx += "\r\nOK\r\n\r\n+APP PDP: 0,DEACTIVE\r\n";
        return;
      }
      pdpActive = true;
      rx += "\r\nOK\r\n\r\n+APP PDP: 0,ACTIVE\r\n";
    } else if (c == "AT+CPOWD=1") {
      if (!answersPowerOff) return;
      powered = false;
      pdpActive = false;
      rx += "\r\nNORMAL POWER DOWN\r\n";
    } else if (c.rfind("AT+CCOAPPARA=", 0) == 0) {
      // Il modem vero risponde ERROR con MID oltre 9999
      size_t at = c.find("\"MID\",");
      bool midOk = at != std::string::npos && atol(c.c_str() + at + 6) <= 9999;
      rx += coapOk && midOk ? "\r\nOK\r\n" : "\r\nERROR\r\n";
    } else if (c == "AT+CNTP") {
      std::string result = ntpResult;
      if (ntpFailures > 0) { ntpFailures--; result = "+CNTP: 62"; }
      rx += "\r\nOK\r\n\r\n" + result + "\r\n";
    } else if (c == "AT+CCLK?") {
      rx += "\r\n+CCLK: " + clock + "\r\n\r\nOK\r\n";
    } else if (c.rfind("AT+CRAI=", 0) == 0) {
      rx += raiSupported ? "\r\nOK\r\n" : "\r\nERROR\r\n";
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
  fakeTickOffset = 0;
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
  CHECK(m.commands[m.commands.size() - 2] == "AT+CRAI=1,0");  // prima dell'ultimo pacchetto
  CHECK(modem.connected());
}

// Tecnologia e bande vanno corrette, e una sola volta: restano salvate nel modem
static void testNetworkConfiguredWhenDifferent() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.networkMode == "2");
  CHECK(m.nbBands == "3,8,20");
  CHECK(m.count("AT+CFUN=0") == 1);
  CHECK(m.count("AT+CFUN=1") == 1);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CMNB=") == 1);
  CHECK(m.count("AT+CBANDCFG=") == 1);
  CHECK(m.count("AT+CFUN=0") == 1);
}

// Con copertura NB-IoT assente, lo sketch puo' chiedere anche Cat-M
static void testNetworkModeChosenBySketch() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683, "3"));
  CHECK(m.networkMode == "3");
  CHECK(m.count("AT+CMNB=3") == 1);
}

// Basta una sola impostazione diversa per scriverle tutte
static void testOnlyBandsDifferent() {
  FakeModem m; reset(m);
  m.apnStored = true;
  m.networkMode = "2";
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.nbBands == "3,8,20");
}

// Scoperto col modem vero: appena sveglio le letture danno ERROR, e il firmware riscriveva il PSM
static void testNothingRewrittenWhileBooting() {
  FakeModem m; reset(m);
  m.apnStored = true;
  m.networkMode = "2";
  m.nbBands = "3,8,20";
  m.storedPsm = "1,,,\"00001100\",\"00000011\"";
  m.bootingCommands = 3;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  // Il PSM si legge dopo la registrazione, quando il modem e' pronto
  std::vector<std::string>& c = m.commands;
  auto attach = std::find(c.begin(), c.end(), "AT+CGATT?");
  auto psm = std::find(c.begin(), c.end(), "AT+CPSMS?");
  CHECK(attach < psm);
  CHECK(m.count("AT+CPSMS=") == 0);
  CHECK(m.count("AT+CFUN") == 0);
  CHECK(m.count("AT+CMNB=") == 0);
}

// Dopo il PSM il modem vero rifiuta AT+CNACT per circa 4 s: 8 tentativi nelle prove
static void testPdpRetriedUntilActive() {
  FakeModem m; reset(m);
  m.cnactFailures = 7;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.connected());
  CHECK(m.count("AT+CNACT=0,1") == 8);
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
}

// Appena uscito dal PSM il modem vero non ha risposto ad AT+CNCFG per qualche secondo
static void testPdpConfigRetriedWhenSilent() {
  FakeModem m; reset(m);
  m.apnStored = true;
  m.networkMode = "2";
  m.nbBands = "3,8,20";
  m.cncfgSilent = 2;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CNCFG=") == 3);
  CHECK(m.count("AT+CNACT=0,1") == 1);
}

static void testPdpGivesUp() {
  FakeModem m; reset(m);
  m.apnStored = true;
  m.networkMode = "2";
  m.nbBands = "3,8,20";
  m.cnactFailures = -1;
  Sim7080 modem(m, 1, HIGH);
  uint32_t start = fakeMillis;
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(!modem.connected());
  CHECK(m.count("AT+CNACT=0,1") >= 10);
  CHECK(fakeMillis - start >= 30000 && fakeMillis - start < 45000);
}

// Il modem normalizza il TAU: con il valore giusto non va riscritto a ogni risveglio
static void testStoredPsmValueMatchesRealModem() {
  FakeModem m; reset(m);
  m.storedPsm = "1,,,\"00001100\",\"00000011\"";
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CPSMS=") == 0);
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
  m.networkMode = "2";
  m.nbBands = "3,8,20";
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

// Bloccato nella ricerca della rete: a meta' attesa la radio viene riavviata
static void testRadioRestartedWhenAttachStuck() {
  FakeModem m; reset(m);
  m.apnStored = true;  // nessun CFUN in configurazione
  m.networkMode = "2";
  m.nbBands = "3,8,20";
  m.attachNeedsRadioRestart = true;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CFUN=0") == 1);
  CHECK(m.count("AT+CFUN=1") == 1);
  CHECK(fakeMillis >= 90000 && fakeMillis < 120000);
}

// Senza rete la radio viene riavviata una volta sola
static void testRadioRestartedOnce() {
  FakeModem m; reset(m);
  m.apnStored = true;  // nessun CFUN in configurazione
  m.networkMode = "2";
  m.nbBands = "3,8,20";
  m.attachAfterPolls = 1000000;
  Sim7080 modem(m, 1, HIGH);
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CFUN=0") == 1);
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
  std::string big(300, 'x');
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
  CHECK(m.count("AT+CRAI=2,0") == 1);  // attende la risposta del server
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

// Con Cat-M il comando puo' non essere supportato: non deve bloccare l'invio
static void testRaiRefusedStillSends() {
  FakeModem m; reset(m);
  m.raiSupported = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(1, "wind", 50, PAYLOAD, false));
  CHECK(m.count("AT+CCOAPACTION") == 1);
}

// Un modem acceso che salta qualche risposta non deve ricevere l'impulso
static void testSlowModemNotPulsed() {
  FakeModem m; reset(m);
  m.ignoreCommands = 2;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(pwrkeyPulses == 0);
  CHECK(m.powered);
}

// Visto sul modem vero: acceso ma muto ai primi AT, l'impulso lo spegne
static void testSilentModemPulsedBackOn() {
  FakeModem m; reset(m);
  m.ignoreCommands = 3;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(pwrkeyPulses == 2);
  CHECK(m.powered);
}

// Il contatore dello sketch parte da millis() e supera presto 9999
static void testMessageIdWrapsBelowModemLimit() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.coapPost(18537, "wind", 50, PAYLOAD, false));
  CHECK(m.last("AT+CCOAPPARA").find("\"MID\",8537,") != std::string::npos);
  CHECK(modem.coapPost(65535, "wind", 50, PAYLOAD, false));
  CHECK(m.last("AT+CCOAPPARA").find("\"MID\",5535,") != std::string::npos);
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

static const char* PSM_CMD = "AT+CPSMS=1,,,\"00001100\",\"00000011\"";

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

// La riga AT non deve superare il limite del modem neanche con il payload massimo
static void testPayloadFitsAtLine() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  std::string longest(128, 'x');  // dimensione del buffer del payload nello sketch
  CHECK(modem.coapPost(65535, "wind", 50, longest.c_str(), true));
  CHECK(m.last("AT+CCOAPPARA").size() <= 559);
}

static std::string hex(const uint8_t* data, size_t len) {
  char buf[3];
  std::string out;
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    out += buf;
  }
  return out;
}

static std::string sha256Hex(const std::string& message) {
  Sha256 hash;
  hash.update((const uint8_t*)message.data(), message.size());
  uint8_t digest[SHA256_DIGEST_BYTES];
  hash.finish(digest);
  return hex(digest, sizeof(digest));
}

static std::string hmacHex(const std::string& key, const std::string& message) {
  uint8_t digest[SHA256_DIGEST_BYTES];
  hmacSha256((const uint8_t*)key.data(), key.size(), (const uint8_t*)message.data(), message.size(), digest);
  return hex(digest, sizeof(digest));
}

// Vettori di FIPS 180-4
static void testSha256Vectors() {
  CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Vettori di RFC 4231: chiavi corte, lunghe come il blocco e piu' lunghe, da cui l'hash
static void testHmacVectors() {
  CHECK(hmacHex(std::string(20, '\x0b'), "Hi There")
        == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(hmacHex("Jefe", "what do ya want for nothing?")
        == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  CHECK(hmacHex(std::string(20, '\xaa'), std::string(50, '\xdd'))
        == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
  CHECK(hmacHex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")
        == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// Stesso vettore dei test del server Go e calcolato con hmac di Python
static const char* TEST_KEY = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char* SIGNED_PAYLOAD = "{\"id\":\"first_anemometer\",\"mean\":10.8,\"max\":15.1,\"dir\":225,\"n\":1793865182,"
                                    "\"sig\":\"684020558e98f5a3ce8e48347f52966a\"}";

static const WindReport SAMPLE_REPORT = {10.8f, 15.1f, 225, 0};

static void testSignedPayloadMatchesVector() {
  char buf[128];
  CHECK(formatSignedWindPayload(buf, sizeof(buf), "first_anemometer", SAMPLE_REPORT, 1793865182, TEST_KEY));
  CHECK(strcmp(buf, SIGNED_PAYLOAD) == 0);
}

static void testSignatureChangesWithEveryField() {
  char base[128], other[128];
  CHECK(formatSignedWindPayload(base, sizeof(base), "a", SAMPLE_REPORT, 1, TEST_KEY));
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", SAMPLE_REPORT, 2, TEST_KEY));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", SAMPLE_REPORT, 1, "altra-chiave"));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
  const WindReport changed = {10.9f, 15.1f, 225, 0};
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", changed, 1, TEST_KEY));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
}

static void testPayloadWorstCaseFitsSketchBuffer() {
  const WindReport worst = {999.9f, 999.9f, 359, 0};
  char buf[128];
  CHECK(formatSignedWindPayload(buf, sizeof(buf), "first_anemometer", worst, 0xFFFFFFFF, TEST_KEY));
}

static void testPayloadTooLong() {
  char buf[128];
  CHECK(formatSignedWindPayload(buf, sizeof(buf), "a", SAMPLE_REPORT, 1, TEST_KEY));
  size_t full = strlen(buf);
  CHECK(!formatSignedWindPayload(buf, 0, "a", SAMPLE_REPORT, 1, TEST_KEY));
  CHECK(!formatSignedWindPayload(buf, full, "a", SAMPLE_REPORT, 1, TEST_KEY));  // un byte in meno
  CHECK(formatSignedWindPayload(buf, full + 1, "a", SAMPLE_REPORT, 1, TEST_KEY));
  CHECK(strlen(buf) == full);
}

// Riferimenti calcolati con il modulo calendar di Python
static void testParseModemClock() {
  uint32_t t = 0;
  CHECK(parseModemClock("\"26/11/05,11:53:02+00\"", t));
  CHECK(t == 1793879582UL);
  CHECK(parseModemClock("\"26/11/05,12:53:02+04\"", t));  // un'ora avanti, stesso istante
  CHECK(t == 1793879582UL);
  CHECK(parseModemClock("\"26/11/05,10:53:02-04\"", t));
  CHECK(t == 1793879582UL);
  CHECK(parseModemClock("\"24/01/01,00:00:00+00\"", t));
  CHECK(t == 1704067200UL);
  CHECK(parseModemClock("\"24/02/29,23:59:59+00\"", t));  // anno bisestile
  CHECK(t == 1709251199UL);
  CHECK(parseModemClock("\"25/03/01,00:00:00+00\"", t));
  CHECK(t == 1740787200UL);
  CHECK(parseModemClock("\"28/12/31,23:59:59+00\"", t));
  CHECK(t == 1861919999UL);
  CHECK(parseModemClock("\"99/12/31,23:59:59+00\"", t));
  CHECK(t == 4102444799UL);
}

static void testParseModemClockRejects() {
  uint32_t t = 0;
  const char* invalid[] = {
    "", "\"", "garbage",
    "\"26/11/05,11:53:02\"",      // senza fuso
    "26/11/05,11:53:02+00",        // senza virgolette
    "\"26/11/05,11:53:02+00",      // senza chiusura
    "\"23/12/31,23:59:59+00\"",   // prima del 2024
    "\"26/13/01,00:00:00+00\"", "\"26/00/01,00:00:00+00\"", "\"26/01/00,00:00:00+00\"",
    "\"26/04/31,00:00:00+00\"", "\"25/02/29,00:00:00+00\"",
    "\"26/01/01,24:00:00+00\"", "\"26/01/01,00:60:00+00\"", "\"26/01/01,00:00:60+00\"",
    "\"26/01/01,00:00:00+97\"", "\"26/01/01,00:00:00*00\"",
  };
  for (const char* text : invalid) {
    bool parsed = parseModemClock(text, t);
    if (parsed) printf("accettato: %s\n", text);
    CHECK(!parsed && text);
  }
}

static const uint32_t HZ = WIND_CLOCK_TICK_HZ;

static void testClockTimestamps() {
  WindClock c;
  CHECK(c.needsSync(0));
  CHECK(!c.usable(0));
  c.sync(1000000, 50 * HZ);
  CHECK(!c.needsSync(50 * HZ));
  CHECK(c.usable(50 * HZ));
  CHECK(c.timestamp(50 * HZ) == 1000000);
  CHECK(c.timestamp(350 * HZ) == 1000300);       // 5 minuti dopo
}

// Il report si chiude prima che il modem si connetta e sincronizzi
static void testClockBeforeSync() {
  WindClock c;
  c.sync(1000000, 400 * HZ);
  CHECK(c.timestamp(100 * HZ) == 999700);
}

static void testClockAlwaysGrows() {
  WindClock c;
  c.sync(1000000, 0);
  CHECK(c.timestamp(10 * HZ) == 1000010);
  c.sync(1000000, 10 * HZ);                      // l'ora arretra di 10 s
  CHECK(c.timestamp(10 * HZ) == 1000011);
  CHECK(c.timestamp(10 * HZ) == 1000012);
  CHECK(c.timestamp(5000 * HZ) == 1004990);
}

static void testClockResyncAndExpiry() {
  const uint32_t HOUR = 3600UL * HZ;
  WindClock c;
  c.sync(1000000, 1000);
  CHECK(!c.needsSync(1000 + 24 * HOUR - 1));
  CHECK(c.needsSync(1000 + 24 * HOUR));
  CHECK(c.usable(1000 + 7 * 24 * HOUR - 1));
  CHECK(!c.usable(1000 + 7 * 24 * HOUR));
}

// I tick ripartono da zero dopo 48,5 giorni: nessun salto, nemmeno a cavallo
static void testClockTickWrap() {
  WindClock c;
  uint32_t beforeWrap = 0xFFFFFFFFUL - 10 * HZ;       // 10 s prima del passaggio
  c.sync(1000000, beforeWrap);
  // Subito dopo la sincronizzazione, con il passaggio ancora da venire
  CHECK(!c.needsSync(beforeWrap + 5 * HZ));
  CHECK(c.usable(beforeWrap + 5 * HZ));
  uint32_t afterWrap = beforeWrap + 40 * HZ;          // 30 s dopo il passaggio
  CHECK(afterWrap < beforeWrap);
  CHECK(!c.needsSync(afterWrap));
  CHECK(c.usable(afterWrap));
  CHECK(c.timestamp(afterWrap) == 1000040);

  // Report chiuso prima del passaggio, sincronizzazione dopo
  WindClock d;
  d.sync(2000000, afterWrap);
  CHECK(d.timestamp(beforeWrap) == 1999960);
}

// Riproduce il millis() della scheda: con le differenze in millisecondi il salto vale 28 ore,
// ed e' il motivo per cui il firmware usa i tick
static void testBoardMillisWouldJump() {
  const uint64_t ticksPerWrap = 1ULL << 32;
  uint32_t lastMs = (uint32_t)((ticksPerWrap - 1) * 1000 / HZ);
  uint32_t firstMs = 0;
  CHECK(lastMs == 4194303999UL);
  CHECK(firstMs - lastMs == 100663297UL);
  uint32_t lastTick = (uint32_t)(ticksPerWrap - 1), firstTick = 0;
  CHECK(firstTick - lastTick == 1);
}

// Un'attesa di risposta a cavallo del passaggio deve durare quanto previsto
static void testTimeoutAcrossTickWrap() {
  FakeModem m; reset(m);
  // Il passaggio arriva dopo 5 s, durante l'attesa di 20 s che segue l'impulso su PWRKEY
  fakeTickOffset = 0xFFFFFFFFUL - ms2tick(5000);
  Sim7080 modem(m, 1, HIGH);
  uint32_t start = fakeMillis;
  m.powered = false;  // nessuna risposta
  m.bootAt = 0xFFFFFFFF;
  activeModem = nullptr;
  CHECK(!modem.connect(APN, HOST, 5683));
  uint32_t waited = fakeMillis - start;
  CHECK(waited >= 40000 && waited < 50000);  // due attese di avvio da 20 s, non zero
}

static void testSyncTime() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  uint32_t t = 0;
  CHECK(modem.syncTime("pool.ntp.org", t));
  CHECK(t == 1793879582UL);
  CHECK(m.count("AT+CNTP=\"pool.ntp.org\",0,0,0") == 1);
}

static void testSyncTimeNtpError() {
  FakeModem m; reset(m);
  m.ntpResult = "+CNTP: 61";
  Sim7080 modem(m, 1, HIGH);
  uint32_t t = 0;
  CHECK(!modem.syncTime("pool.ntp.org", t));
  CHECK(m.count("AT+CCLK?") == 0);  // l'orologio non e' stato aggiornato
  CHECK(m.count("AT+CNTP") - m.count("AT+CNTP=") == 3);  // tutti i tentativi
  CHECK(fakeMillis < 10000);        // riconosce l'errore senza attendere il timeout
}

static void testSyncTimeRetriesDns() {
  FakeModem m; reset(m);
  m.ntpFailures = 1;  // come al risveglio dal PSM: il primo tentativo non risolve il nome
  Sim7080 modem(m, 1, HIGH);
  uint32_t t = 0;
  CHECK(modem.syncTime("pool.ntp.org", t));
  CHECK(t == 1793879582UL);
  CHECK(m.count("AT+CNTP") - m.count("AT+CNTP=") == 2);
}

static void testSyncTimeBadClock() {
  FakeModem m; reset(m);
  m.clock = "\"20/01/06,00:00:00+00\"";  // data di default di un modem non sincronizzato
  Sim7080 modem(m, 1, HIGH);
  uint32_t t = 0;
  CHECK(!modem.syncTime("pool.ntp.org", t));
}

static bool close(float a, float b) { return a > b - 0.001f && a < b + 0.001f; }

static void testGustNeedsFullWindow() {
  RollingGust g;
  float rate = -1.0f;
  CHECK(!g.add(5, 1.0f, rate));
  CHECK(!g.add(5, 1.0f, rate));
  CHECK(rate == -1.0f);
  CHECK(g.add(5, 1.0f, rate));
  CHECK(close(rate, 5.0f));
}

static void testGustSlides() {
  RollingGust g;
  float rate;
  const uint32_t pulses[] = {0, 3, 9, 6, 0, 0};
  const float expected[] = {0, 0, 4.0f, 6.0f, 5.0f, 2.0f};
  for (int i = 0; i < 6; i++) {
    bool full = g.add(pulses[i], 1.0f, rate);
    CHECK(full == (i >= 2));
    if (full) CHECK(close(rate, expected[i]));
  }
}

// Una finestra allineata (0,3,9 e 6,0,0) darebbe 4 e 2: la raffica vera e' 6
static void testGustNotSplitAcrossBlocks() {
  RollingGust g;
  float rate, best = 0.0f;
  const uint32_t pulses[] = {0, 3, 9, 6, 0, 0};
  for (uint32_t p : pulses) {
    if (g.add(p, 1.0f, rate) && rate > best) best = rate;
  }
  CHECK(close(best, 6.0f));
}

// Un impulso spurio su 3 s pesa un terzo rispetto a un campione da 1 s
static void testGustDampsSpuriousPulse() {
  RollingGust g;
  float rate;
  g.add(10, 1.0f, rate);
  g.add(10, 1.0f, rate);
  CHECK(g.add(11, 1.0f, rate));
  CHECK(close(rate, 31.0f / 3.0f));
}

// Un campione in ritardo non deve creare una falsa raffica
static void testGustUsesMeasuredTime() {
  RollingGust g;
  float rate;
  g.add(10, 1.0f, rate);
  g.add(20, 2.0f, rate);
  CHECK(g.add(10, 1.0f, rate));
  CHECK(close(rate, 10.0f));
}

int main() {
  testColdBootConnectAndPost();
  testNetworkConfiguredWhenDifferent();
  testNetworkModeChosenBySketch();
  testOnlyBandsDifferent();
  testNothingRewrittenWhileBooting();
  testPdpRetriedUntilActive();
  testPdpConfigRetriedWhenSilent();
  testPdpGivesUp();
  testStoredPsmValueMatchesRealModem();
  testRunningModemNotPulsed();
  testStoredApnSkipsCfun();
  testModemDead();
  testSimNotReady();
  testNoNetworkTimesOut();
  testRadioRestartedWhenAttachStuck();
  testRadioRestartedOnce();
  testPostParaErrorMarksDisconnected();
  testActionErrorMarksDisconnected();
  testOversizedPayloadRejected();
  testConfirmableAcknowledged();
  testConfirmableServerSilent();
  testUrcsAreIgnored();
  testUrcBetweenPostsDetected();
  testRaiRefusedStillSends();
  testSlowModemNotPulsed();
  testSilentModemPulsedBackOn();
  testMessageIdWrapsBelowModemLimit();
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
  testPayloadFitsAtLine();
  testSha256Vectors();
  testHmacVectors();
  testSignedPayloadMatchesVector();
  testSignatureChangesWithEveryField();
  testPayloadWorstCaseFitsSketchBuffer();
  testPayloadTooLong();
  testParseModemClock();
  testParseModemClockRejects();
  testClockTimestamps();
  testClockBeforeSync();
  testClockAlwaysGrows();
  testClockResyncAndExpiry();
  testClockTickWrap();
  testBoardMillisWouldJump();
  testTimeoutAcrossTickWrap();
  testSyncTime();
  testSyncTimeNtpError();
  testSyncTimeRetriesDns();
  testSyncTimeBadClock();
  testGustNeedsFullWindow();
  testGustSlides();
  testGustNotSplitAcrossBlocks();
  testGustDampsSpuriousPulse();
  testGustUsesMeasuredTime();
  printf(failures ? "%d test falliti\n" : "tutti i test superati\n", failures);
  return failures ? 1 : 0;
}
