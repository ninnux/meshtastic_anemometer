// Test sul computer della codifica CoAP e del driver SIM7080 con un modem simulato.
// Dalla cartella anemometro_sim7080_coap:
//   g++ -std=c++17 -Itest -I. test/test_sim7080.cpp sim7080.cpp -o /tmp/test_sim7080 && /tmp/test_sim7080
// Se su macOS non trova gli header C++, aggiungere:
//   -isysroot $(xcrun --show-sdk-path) -I$(xcrun --show-sdk-path)/usr/include/c++/v1
// Con --dump stampa in esadecimale il datagramma per il test contro un server reale.
#include <vector>
#include "Arduino.h"
#include "coap.h"
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
  bool openOk = true;
  bool sendOk = true;
  std::string urcBeforeReply;
  std::vector<std::string> commands;
  std::vector<uint8_t> datagram;

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
  void togglePower() {
    powered = !powered;
    if (powered) bootAt = fakeMillis + 3000;
  }
  int count(const std::string& prefix) const {
    int n = 0;
    for (auto& c : commands) if (c.rfind(prefix, 0) == 0) n++;
    return n;
  }

private:
  std::string rx, cmd;
  size_t dataLeft = 0;

  void onByte(uint8_t b) {
    if (dataLeft > 0) {
      datagram.push_back(b);
      if (--dataLeft == 0) rx += sendOk ? "\r\nOK\r\n" : "\r\nERROR\r\n";
      return;
    }
    if (b != '\r') { cmd += (char)b; return; }
    std::string c = cmd;
    cmd.clear();
    if (!powered || fakeMillis < bootAt) return;
    commands.push_back(c);
    rx += urcBeforeReply;
    reply(c);
  }

  void reply(const std::string& c) {
    if (c == "AT+CPIN?") {
      rx += simReady ? "\r\n+CPIN: READY\r\n\r\nOK\r\n" : "\r\n+CPIN: NOT READY\r\n\r\nOK\r\n";
    } else if (c == "AT+CGATT?") {
      rx += attachAfterPolls-- <= 0 ? "\r\n+CGATT: 1\r\n\r\nOK\r\n" : "\r\n+CGATT: 0\r\n\r\nOK\r\n";
    } else if (c == "AT+CNACT?") {
      rx += pdpActive ? "\r\n+CNACT: 0,1,\"10.0.0.2\"\r\n" : "\r\n+CNACT: 0,0,\"0.0.0.0\"\r\n";
      rx += "+CNACT: 1,0,\"0.0.0.0\"\r\n\r\nOK\r\n";
    } else if (c == "AT+CNACT=0,1") {
      pdpActive = true;
      rx += "\r\nOK\r\n\r\n+APP PDP: 0,ACTIVE\r\n";
    } else if (c.rfind("AT+CAOPEN=", 0) == 0) {
      rx += openOk ? "\r\n+CAOPEN: 0,0\r\n\r\nOK\r\n" : "\r\n+CAOPEN: 0,1\r\n\r\nOK\r\n";
    } else if (c.rfind("AT+CASEND=0,", 0) == 0) {
      if (!sendOk) { rx += "\r\nERROR\r\n"; return; }
      dataLeft = atoi(c.c_str() + strlen("AT+CASEND=0,"));
      rx += "\r\n> ";
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

static std::vector<uint8_t> encodeSample(uint16_t id) {
  const char* payload = "{\"mean\":10.8,\"max\":15.1,\"dir\":225}";
  uint8_t buf[96];
  size_t n = coapBuildNonPost(buf, sizeof(buf), id, "wind/first_anemometer",
                              COAP_CONTENT_FORMAT_JSON, payload, strlen(payload));
  return std::vector<uint8_t>(buf, buf + n);
}

static void testCoapBytes() {
  uint8_t buf[64];
  size_t n = coapBuildNonPost(buf, sizeof(buf), 0x1234, "wind/abc", 50, "x", 1);
  const uint8_t expected[] = {0x50, 0x02, 0x12, 0x34, 0xB4, 'w', 'i', 'n', 'd',
                              0x03, 'a', 'b', 'c', 0x11, 50, 0xFF, 'x'};
  CHECK(n == sizeof(expected));
  CHECK(memcmp(buf, expected, sizeof(expected)) == 0);
}

static void testCoapLongSegment() {
  uint8_t buf[64];
  size_t n = coapBuildNonPost(buf, sizeof(buf), 1, "first_anemometer", 50, "", 0);
  const uint8_t head[] = {0x50, 0x02, 0x00, 0x01, 0xBD, 16 - 13};
  CHECK(n == 6 + 16 + 2);  // senza payload non c'e' il marcatore 0xFF
  CHECK(memcmp(buf, head, sizeof(head)) == 0);
  CHECK(buf[6 + 16] == 0x11);
}

static void testCoapRejects() {
  uint8_t buf[96];
  for (size_t cap = 0; cap < encodeSample(1).size(); cap++) {
    CHECK(coapBuildNonPost(buf, cap, 1, "wind/first_anemometer", 50,
                           "{\"mean\":10.8,\"max\":15.1,\"dir\":225}", 34) == 0);
  }
  CHECK(coapBuildNonPost(buf, sizeof(buf), 1, "wind//x", 50, "", 0) == 0);
  CHECK(coapBuildNonPost(buf, sizeof(buf), 1, "", 50, "", 0) == 0);
}

static void testColdBootConnectAndSend() {
  FakeModem m; reset(m);
  m.powered = false;
  m.attachAfterPolls = 2;
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.connected());
  CHECK(pwrkeyPulses == 1);
  CHECK(m.count("AT+CGDCONT=1,\"IP\",\"internet\"") == 1);
  CHECK(m.count("AT+CNCFG=0,1,\"internet\"") == 1);
  CHECK(m.count("AT+CAOPEN=0,0,\"UDP\",\"192.0.2.1\",5683") == 1);
  CHECK(m.count("AT+CGATT?") == 3);

  auto d = encodeSample(7);
  CHECK(modem.send(d.data(), d.size()));
  CHECK(m.datagram == d);
  char expected[32];
  snprintf(expected, sizeof(expected), "AT+CASEND=0,%u", (unsigned)d.size());
  CHECK(m.count(expected) == 1);
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

static void testApnConfiguredOncePerBoot() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(modem.connect(APN, HOST, 5683));
  CHECK(m.count("AT+CGDCONT") == 1);
}

static void testModemDead() {
  FakeModem m; reset(m);
  m.powered = false;
  m.bootAt = 0xFFFFFFFF;
  Sim7080 modem(m, -1, HIGH);
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

static void testOpenFails() {
  FakeModem m; reset(m);
  m.openOk = false;
  Sim7080 modem(m, 1, HIGH);
  CHECK(!modem.connect(APN, HOST, 5683));
  CHECK(!modem.connected());
  CHECK(fakeMillis < 30000);  // deve riconoscere l'errore, non scadere il timeout
}

static void testSendErrorMarksDisconnected() {
  FakeModem m; reset(m);
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  m.sendOk = false;
  auto d = encodeSample(1);
  CHECK(!modem.send(d.data(), d.size()));
  CHECK(!modem.connected());
  CHECK(m.datagram.empty());
}

static void testUrcsAreIgnored() {
  FakeModem m; reset(m);
  m.urcBeforeReply = "\r\n+CADATAIND: 0\r\n";
  Sim7080 modem(m, 1, HIGH);
  CHECK(modem.connect(APN, HOST, 5683));
  auto d = encodeSample(1);
  CHECK(modem.send(d.data(), d.size()));
  CHECK(m.datagram == d);
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

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
    for (uint8_t b : encodeSample(0x4242)) printf("%02x", b);
    printf("\n");
    return 0;
  }
  testCoapBytes();
  testCoapLongSegment();
  testCoapRejects();
  testColdBootConnectAndSend();
  testRunningModemNotPulsed();
  testApnConfiguredOncePerBoot();
  testModemDead();
  testSimNotReady();
  testNoNetworkTimesOut();
  testOpenFails();
  testSendErrorMarksDisconnected();
  testUrcsAreIgnored();
  testCheckLink();
  printf(failures ? "%d test falliti\n" : "tutti i test superati\n", failures);
  return failures ? 1 : 0;
}
