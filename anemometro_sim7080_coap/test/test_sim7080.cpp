// Test sul computer del driver SIM7080 con un modem simulato.
// Dalla cartella anemometro_sim7080_coap:
//   g++ -std=c++17 -Itest -I. test/test_sim7080.cpp sim7080.cpp -o /tmp/test_sim7080 && /tmp/test_sim7080
// Se su macOS non trova gli header C++, aggiungere:
//   -isysroot $(xcrun --show-sdk-path) -I$(xcrun --show-sdk-path)/usr/include/c++/v1
#include <stdlib.h>
#include <vector>
#include "Arduino.h"
#include "sim7080.h"
#include "hmac_sha256.h"
#include "wind_report.h"

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
  bool raiSupported = true;
  int ignoreCommands = 0;
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

static const char* PSM_CMD = "AT+CPSMS=1,,,\"00100010\",\"00000001\"";

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
static const char* SIGNED_PAYLOAD = "{\"id\":\"first_anemometer\",\"mean\":10.8,\"max\":15.1,\"dir\":225,\"n\":65537,"
                                    "\"sig\":\"3a482c440614dcc4d24ea0c6a99eed20\"}";

static const WindReport SAMPLE_REPORT = {10.8f, 15.1f, 225};

static void testSignedPayloadMatchesVector() {
  char buf[128];
  CHECK(formatSignedWindPayload(buf, sizeof(buf), "first_anemometer", SAMPLE_REPORT, 65537, TEST_KEY));
  CHECK(strcmp(buf, SIGNED_PAYLOAD) == 0);
}

static void testSignatureChangesWithEveryField() {
  char base[128], other[128];
  CHECK(formatSignedWindPayload(base, sizeof(base), "a", SAMPLE_REPORT, 1, TEST_KEY));
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", SAMPLE_REPORT, 2, TEST_KEY));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", SAMPLE_REPORT, 1, "altra-chiave"));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
  const WindReport changed = {10.9f, 15.1f, 225};
  CHECK(formatSignedWindPayload(other, sizeof(other), "a", changed, 1, TEST_KEY));
  CHECK(strcmp(strstr(base, "sig"), strstr(other, "sig")) != 0);
}

static void testPayloadWorstCaseFitsSketchBuffer() {
  const WindReport worst = {999.9f, 999.9f, 359};
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

static void testMessageCounterGrows() {
  MessageCounter c;
  CHECK(!c.ready());
  c.setEpoch(1);
  CHECK(c.ready());
  CHECK(c.next() == 65536);
  CHECK(c.next() == 65537);
  c.setEpoch(2);  // riavvio: il valore supera sempre quelli dell'epoca precedente
  CHECK(c.next() == 131072);
}

static void testMessageCounterNeedsNewEpochWhenExhausted() {
  MessageCounter c;
  c.setEpoch(1);
  uint32_t last = 0;
  for (uint32_t i = 0; i < 65536; i++) {
    CHECK(c.ready());
    uint32_t value = c.next();
    if (i > 0) CHECK(value > last);
    last = value;
  }
  CHECK(!c.ready());
  CHECK(last == 0x1FFFF);
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
  testRaiRefusedStillSends();
  testSlowModemNotPulsed();
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
  testMessageCounterGrows();
  testMessageCounterNeedsNewEpochWhenExhausted();
  testGustNeedsFullWindow();
  testGustSlides();
  testGustNotSplitAcrossBlocks();
  testGustDampsSpuriousPulse();
  testGustUsesMeasuredTime();
  printf(failures ? "%d test falliti\n" : "tutti i test superati\n", failures);
  return failures ? 1 : 0;
}
