// ============================================================
// meshtastic_davis_anemometer.ino
// Board: Seeed XIAO nRF52840
// BSP:   Seeed nRF52 Boards >= 1.1.0
// ============================================================

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "Meshtastic.h"
#include <bluefruit.h>

// ── Pin ──────────────────────────────────────────────────────
#define WindSensorPin  2
#define WindVanePin    A4
#define VaneOffset     0


// ── Meshtastic ───────────────────────────────────────────────
#define DEST_NODE      0x12ab2251
#define SERIAL_RX_PIN  7
#define SERIAL_TX_PIN  6
#define BAUD_RATE      38400
#define NODE_NAME      "first_anemometer"

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
volatile bool     IsSampleRequired      = false;
volatile bool     IsMeshtasticRequired  = false;
volatile bool     IsBLEUpdateRequired   = false;
volatile uint8_t  TimerCount            = 0;
volatile uint32_t Rotations             = 0;
volatile uint32_t ContactBounceTime     = 0;

float WindSpeed         = 0.0f;
float WindSpeedMean     = 0.0f;
float WindSpeedMax      = 0.0f;
float WindSpeedSum      = 0.0f;
int   WindSpeedDiv      = 0;
int   vaneValue         = 0;
int   windDirection     = 0;
int   windCalDirection  = 0;
int   lastWindDirection = 0;
char  windCompassDirection[4] = "N";

// ── Prototipi ────────────────────────────────────────────────
void isr_timer_cb(TimerHandle_t xTimerID);
void isr_rotation();
void getWindDirection();
void setupBLE();
void updateBLE();
void startAdvertising();
void stopAdvertising();

// ============================================================
// SETUP
// ============================================================
void setup() {
  // pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
  // pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  // pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);

  Bluefruit.autoConnLed(false);
  Bluefruit.setConnLedInterval(0);

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && (millis() - t) < 3000);

  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, BAUD_RATE);

  pinMode(WindSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(WindSensorPin), isr_rotation, FALLING);

  setupBLE();

  lastAdvStart = millis() - ADV_PERIOD_MS;

  sampleTimer.begin(500, isr_timer_cb);
  sampleTimer.start();

  Serial.println("Sistema avviato.");
}

// ============================================================
// LOOP
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
  while (!IsSampleRequired) {
    __WFE();
    __SEV();
    __WFE();
  }
  IsSampleRequired = false;

  // ── Calcolo vento ─────────────────────────────────────────
  getWindDirection();

  if (abs(windCalDirection - lastWindDirection) > 5)
    lastWindDirection = windCalDirection;

  noInterrupts();
  uint32_t rot = Rotations;
  Rotations = 0;
  interrupts();

  WindSpeed = rot * 0.9f * 0.868976f;
  WindSpeedDiv++;
  WindSpeedSum += WindSpeed;
  WindSpeedMean = WindSpeedSum / WindSpeedDiv;
  if (WindSpeed > WindSpeedMax) WindSpeedMax = WindSpeed;

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

  // ── Meshtastic ogni 15s ───────────────────────────────────
  if (IsMeshtasticRequired) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "Name:%s Last:%.1f Mean:%.1f Max:%.1f kn Dir:%d %s",
             NODE_NAME,WindSpeed, WindSpeedMean, WindSpeedMax,
             windCalDirection, windCompassDirection);
    mt_send_text(buf, DEST_NODE, 0);
    IsMeshtasticRequired = false;
    WindSpeedSum  = 0;
    WindSpeedDiv  = 0;
    WindSpeedMean = 0;
    WindSpeedMax  = 0;
  }
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
// ISR – Timer ogni 500 ms
// ============================================================
void isr_timer_cb(TimerHandle_t xTimerID) {
  (void)xTimerID;
  TimerCount++;

  if (TimerCount >= 2) {
    IsSampleRequired    = true;
    IsBLEUpdateRequired = true;
  }

  if (TimerCount >= 30) {
    IsMeshtasticRequired = true;
    TimerCount = 0;
  }
}

// ============================================================
// ISR – Rotazione anemometro
// ============================================================
void isr_rotation() {
  uint32_t now = millis();
  if ((now - ContactBounceTime) > 15) {
    Rotations++;
    ContactBounceTime = now;
    Serial.println("magnete");
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