// ============================================================
// anemometro_sim7080_coap.ino
// Board: Seeed XIAO nRF52840
// BSP:   Seeed nRF52 Boards >= 1.1.0
// Modem: SIMCom SIM7080G (Cat-M / NB-IoT)
// ============================================================

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>
#include <math.h>
#include "sim7080.h"
#include "wind_report.h"

// ── Pin ──────────────────────────────────────────────────────
#define WindSensorPin  2
#define WindVanePin    A3
#define VaneOffset     0

// ── Modem (Serial1: RX=D7, TX=D6) ────────────────────────────
#define MODEM_BAUD_RATE     115200
#define MODEM_PWRKEY_PIN    1
#define MODEM_PWRKEY_ACTIVE HIGH  // HIGH con transistor sulla scheda, LOW se diretto
#define MODEM_APN           "internet"

// ── CoAP ─────────────────────────────────────────────────────
#define COAP_SERVER_IP   "192.0.2.1"  // IPv4 del server con collector_coap.py
#define COAP_SERVER_PORT 5683
#define NODE_NAME        "first_anemometer"
#define COAP_PATH        "wind"
#define COAP_CONFIRMABLE false  // true: rileva anche un server spento, con piu' traffico
#define COAP_CONTENT_FORMAT_JSON 50

// ── Invio ────────────────────────────────────────────────────
#define REPORT_PERIOD_S 300
#define FAILS_TO_RESET  3  // errori consecutivi prima dello spegnimento completo

// ── BLE ──────────────────────────────────────────────────────
BLEService        windService("A0010000-0001-0001-0001-000000000001");
BLECharacteristic bleWindData("A0010000-0001-0001-0001-000000000002",
                               CHR_PROPS_READ | CHR_PROPS_NOTIFY, 32);

// ── Advertising periodico ────────────────────────────────────
#define ADV_WINDOW_MS   10000UL
#define ADV_PERIOD_MS  30000UL

uint32_t lastAdvStart  = 0;
bool     isAdvertising = false;

// ── SoftwareTimer ────────────────────────────────────────────
SoftwareTimer sampleTimer;

// ── Variabili anemometro ─────────────────────────────────────
volatile bool     IsReportRequired      = false;
volatile bool     IsBLEUpdateRequired   = false;
volatile uint16_t TimerCount            = 0;
volatile uint32_t Rotations             = 0;
volatile uint32_t ContactBounceTime     = 0;

float WindSpeed         = 0.0f;
float WindSpeedMean     = 0.0f;
float WindSpeedMax      = 0.0f;
float WindSpeedSum      = 0.0f;
float WindTimeSum       = 0.0f;
uint32_t LastSampleMs   = 0;
float WindDirSinSum     = 0.0f;
float WindDirCosSum     = 0.0f;
int   vaneValue         = 0;
int   windDirection     = 0;
int   windCalDirection  = 0;
char  windCompassDirection[4] = "N";

// ── Passaggio dati al task del modem ─────────────────────────
SemaphoreHandle_t sampleSemaphore;
QueueHandle_t     reportQueue;  // un solo elemento: se il modem e' occupato vince l'ultimo

Sim7080 modem(Serial1, MODEM_PWRKEY_PIN, MODEM_PWRKEY_ACTIVE);

// ── Prototipi ────────────────────────────────────────────────
void isr_timer_cb(TimerHandle_t xTimerID);
void isr_rotation();
void getWindDirection();
int  meanWindDirection();
void queueReport();
void modemLoop();
bool sendReport(const WindReport& report);
void setupBLE();
void updateBLE();
void startAdvertising();
void stopAdvertising();

// ============================================================
// SETUP
// ============================================================
void setup() {
  Bluefruit.autoConnLed(false);
  Bluefruit.setConnLedInterval(0);

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && (millis() - t) < 3000);

  modem.begin();

  pinMode(WindSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(WindSensorPin), isr_rotation, FALLING);
  LastSampleMs = millis();

  // Il default e' 0-3.6 V: con la banderuola a 3.3 V la lettura si fermerebbe
  // attorno a 330°. Con VDD come riferimento non dipende dalla tensione.
  analogReference(AR_VDD4);

  setupBLE();

  lastAdvStart = millis() - ADV_PERIOD_MS;

  sampleSemaphore = xSemaphoreCreateBinary();
  reportQueue     = xQueueCreate(1, sizeof(WindReport));

  // I comandi AT bloccano per secondi o minuti: stanno in un task separato,
  // cosi' il campionamento non si ferma e le rotazioni accumulate durante
  // un invio non diventano una falsa raffica nel campione successivo.
  Scheduler.startLoop(modemLoop);

  sampleTimer.begin(500, isr_timer_cb);
  sampleTimer.start();

  Serial.println("Sistema avviato.");
}

// ============================================================
// LOOP – Campionamento
// ============================================================
void loop() {
  uint32_t now = millis();

  // ── Gestione advertising periodico ───────────────────────
  if (!Bluefruit.connected()) {
    if (!isAdvertising && (now - lastAdvStart >= ADV_PERIOD_MS)) {
      lastAdvStart = now;
      startAdvertising();
    }
    if (isAdvertising && (now - lastAdvStart >= ADV_WINDOW_MS)) {
      stopAdvertising();
    }
  }

  // ── Sleep ─────────────────────────────────────────────────
  // In attesa il task e' sospeso e FreeRTOS mette in sleep la CPU
  // quando anche il task del modem e' fermo.
  xSemaphoreTake(sampleSemaphore, portMAX_DELAY);

  // ── Calcolo vento ─────────────────────────────────────────
  getWindDirection();

  uint32_t sampleMs = millis();
  noInterrupts();
  float rot = (float) Rotations/ 3.0f;
  Rotations = 0;
  interrupts();

  // Si usa il tempo misurato: un campione in ritardo conterrebbe piu' rotazioni
  // e, diviso per 1 s fisso, risulterebbe una falsa raffica.
  float elapsedS = (sampleMs - LastSampleMs) / 1000.0f;
  LastSampleMs = sampleMs;

  WindSpeed = rot * 0.9f * 3.0f * 0.868976f / elapsedS;
  WindSpeedSum += WindSpeed * elapsedS;
  WindTimeSum  += elapsedS;
  WindSpeedMean = WindSpeedSum / WindTimeSum;
  if (WindSpeed > WindSpeedMax) WindSpeedMax = WindSpeed;

  float dirRad = radians(windCalDirection);
  WindDirSinSum += sinf(dirRad);
  WindDirCosSum += cosf(dirRad);

  Serial.print("WindSpeed (kn): ");  Serial.print(WindSpeed);
  Serial.print(" Mean (kn): ");      Serial.print(WindSpeedMean);
  Serial.print(" Max (kn): ");       Serial.print(WindSpeedMax);
  Serial.print(" | Dir: ");          Serial.print(windCalDirection);
  Serial.print("° (");               Serial.print(windCompassDirection);
  Serial.println(")");

  // ── Aggiorna BLE ──────────────────────────────────────────
  if (IsBLEUpdateRequired) {
    IsBLEUpdateRequired = false;
    if (Bluefruit.connected()) {
      updateBLE();
    }
  }

  // ── Report ogni 5 minuti ──────────────────────────────────
  if (IsReportRequired) {
    IsReportRequired = false;
    queueReport();
    WindSpeedSum  = 0;
    WindTimeSum   = 0;
    WindSpeedMean = 0;
    WindSpeedMax  = 0;
    WindDirSinSum = 0;
    WindDirCosSum = 0;
  }
}

// ============================================================
// Report – Prepara i valori del periodo per il task del modem
// ============================================================
void queueReport() {
  WindReport report = { WindSpeedMean, WindSpeedMax, meanWindDirection() };
  xQueueOverwrite(reportQueue, &report);
}

// Media vettoriale: la media aritmetica di 350° e 10° darebbe 180° invece di 0°
int meanWindDirection() {
  int dir = (int)lroundf(degrees(atan2f(WindDirSinSum, WindDirCosSum)));
  return (dir + 360) % 360;
}

// ============================================================
// Task modem – Connessione e invio
// ============================================================
// A ogni report il modem viene svegliato, usato e lasciato tornare in PSM
void modemLoop() {
  static uint8_t failures = 0;

  WindReport report;
  if (xQueueReceive(reportQueue, &report, portMAX_DELAY) != pdTRUE) return;

  Serial1.begin(MODEM_BAUD_RATE);

  bool sent = false;
  if (modem.connect(MODEM_APN, COAP_SERVER_IP, COAP_SERVER_PORT)) {
    sent = sendReport(report);
  }

  if (sent) {
    failures = 0;
  } else if (++failures >= FAILS_TO_RESET) {
    failures = 0;
    modem.powerOff();
    Serial1.end();
    return;
  }

  modem.standby();
  Serial1.end();  // la UART attiva impedisce alla CPU il sonno piu' profondo
}

bool sendReport(const WindReport& report) {
  // Il modem riparte ogni volta da capo: un contatore qui evita che il server
  // scambi i messaggi dopo una riaccensione per duplicati di quelli precedenti.
  static uint16_t messageId = (uint16_t)millis();

  char payload[96];
  if (!formatWindPayload(payload, sizeof(payload), NODE_NAME, report)) {
    Serial.println("Report troppo lungo");
    return false;
  }

  bool sent = modem.coapPost(messageId++, COAP_PATH, COAP_CONTENT_FORMAT_JSON,
                             payload, COAP_CONFIRMABLE);
  Serial.print(sent ? "CoAP inviato: " : "CoAP non inviato: ");
  Serial.println(payload);
  return sent;
}

// ============================================================
// BLE – Setup
// ============================================================
void setupBLE() {
  Bluefruit.begin();
  Bluefruit.setTxPower(0);
  Bluefruit.setName("WindSensor");

  windService.begin();

  bleWindData.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  bleWindData.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  bleWindData.setMaxLen(32);
  bleWindData.begin();
  bleWindData.write("S:0.0 M:0.0 X:0.0 D:0 N", 24);

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(windService);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.setInterval(160, 3200);
  Bluefruit.Advertising.setFastTimeout(30);

  Serial.println("BLE configurato, radio spento.");
}

// ============================================================
// BLE – Avvia advertising
// ============================================================
void startAdvertising() {
  if (!isAdvertising) {
    Bluefruit.Advertising.start(0);
    isAdvertising = true;
    Serial.println("BLE advertising ON");
  }
}

// ============================================================
// BLE – Ferma advertising
// ============================================================
void stopAdvertising() {
  if (isAdvertising && !Bluefruit.connected()) {
    Bluefruit.Advertising.stop();
    isAdvertising = false;
    Serial.println("BLE advertising OFF");
  }
}

// ============================================================
// BLE – Aggiorna caratteristica con stringa leggibile
// ============================================================
void updateBLE() {
  char buf[32];
  snprintf(buf, sizeof(buf), "S:%.1f M:%.1f X:%.1f D:%d %s",
           WindSpeed, WindSpeedMean, WindSpeedMax,
           windCalDirection, windCompassDirection);

  bleWindData.write(buf, strlen(buf));

  if (Bluefruit.connected()) {
    bleWindData.notify(buf, strlen(buf));
  }
}

// ============================================================
// Timer ogni 500 ms
// ============================================================
void isr_timer_cb(TimerHandle_t xTimerID) {
  (void)xTimerID;
  TimerCount++;

  // report ogni REPORT_PERIOD_S (2 tick al secondo), impostato prima di risvegliare il loop
  if (TimerCount >= REPORT_PERIOD_S * 2) {
    IsReportRequired = true;
    TimerCount = 0;
  }

  // campione ogni 2 tick (1 s)
  if (TimerCount % 2 == 0) {
    IsBLEUpdateRequired = true;
    xSemaphoreGive(sampleSemaphore);
  }
}

// ============================================================
// ISR – Rotazione anemometro
// ============================================================
void isr_rotation() {
  uint32_t now = millis();
  if ((now - ContactBounceTime) > 55) {
    Rotations++;
    ContactBounceTime = now;
  }
}

// ============================================================
// Direzione vento
// ============================================================
void getWindDirection() {
  vaneValue        = analogRead(WindVanePin);
  windDirection    = map(vaneValue, 0, 1023, 0, 360);
  windCalDirection = windDirection + VaneOffset;

  if (windCalDirection > 360) windCalDirection -= 360;
  if (windCalDirection <   0) windCalDirection += 360;

  if      (windCalDirection <  22) strncpy(windCompassDirection, "N",  4);
  else if (windCalDirection <  67) strncpy(windCompassDirection, "NE", 4);
  else if (windCalDirection < 112) strncpy(windCompassDirection, "E",  4);
  else if (windCalDirection < 157) strncpy(windCompassDirection, "SE", 4);
  else if (windCalDirection < 212) strncpy(windCompassDirection, "S",  4);
  else if (windCalDirection < 247) strncpy(windCompassDirection, "SW", 4);
  else if (windCalDirection < 292) strncpy(windCompassDirection, "W",  4);
  else if (windCalDirection < 337) strncpy(windCompassDirection, "NW", 4);
  else                             strncpy(windCompassDirection, "N",  4);
}
