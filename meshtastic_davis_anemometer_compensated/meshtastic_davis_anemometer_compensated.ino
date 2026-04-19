// ============================================================
// meshtastic_davis_anemometer.ino
// Board: Seeed XIAO nRF52840 Sense
// BSP:   Seeed nRF52 Boards >= 1.1.0
//
// AGGIUNTA: compensazione orientamento banderuola via
//           magnetometro QMC5883L + accelerometro LSM6DS3
//           con tilt compensation completa.
//
// NOTE: Preferences.h (solo ESP32) sostituita con
//       Adafruit_LittleFS / InternalFileSystem (nRF52).
// ============================================================

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "Meshtastic.h"
#include <bluefruit.h>
#include <Wire.h>
#include <QMC5883LCompass.h>
#include <LSM6DS3.h>
#include <math.h>

// ── LittleFS namespace ───────────────────────────────────────
using namespace Adafruit_LittleFS_Namespace;

// ── File calibrazione bussola ────────────────────────────────
#define CALIB_FILE "/compass_calib.bin"

struct CompassCalib {
  int xMin, xMax, yMin, yMax, zMin, zMax;
};

// ── Pin ──────────────────────────────────────────────────────
#define WindSensorPin  2
#define WindVanePin    A4

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

// ── Sensori magnetometro + accelerometro ─────────────────────
QMC5883LCompass compass;
LSM6DS3         imu(I2C_MODE, 0x6A);

// Heading magnetico compensato (0–359°), aggiornato ogni campione
int deviceHeading = 0;

// ── Prototipi ────────────────────────────────────────────────
void isr_timer_cb(TimerHandle_t xTimerID);
void isr_rotation();
void getWindDirection();
void setupBLE();
void updateBLE();
void startAdvertising();
void stopAdvertising();
void setupCompass();
void updateDeviceHeading();
void imuSleep();
void imuWake();

// ============================================================
// SETUP
// ============================================================
void setup() {
  Bluefruit.autoConnLed(false);
  Bluefruit.setConnLedInterval(0);

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && (millis() - t) < 3000);

  Wire.begin();
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, BAUD_RATE);

  pinMode(WindSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(WindSensorPin), isr_rotation, FALLING);

  setupBLE();
  setupCompass();

  // Parti con sensori in sleep
  imuSleep();
  compass.setMode(0x00, 0x00, 0x00, 0x00);

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

  // ── Aggiorna heading dispositivo (magnetometro+IMU) ───────
  updateDeviceHeading();

  // ── Calcolo direzione vento (banderuola + compensazione) ──
  getWindDirection();

  if (abs(windCalDirection - lastWindDirection) > 5)
    lastWindDirection = windCalDirection;

  // ── Calcolo velocità ──────────────────────────────────────
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
  Serial.print(") | Heading: ");     Serial.print(deviceHeading);
  Serial.println("°");

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
             NODE_NAME, WindSpeed, WindSpeedMean, WindSpeedMax,
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
// COMPASS – Setup e calibrazione con LittleFS
// ============================================================
void setupCompass() {
  imu.begin();
  compass.init();
  InternalFS.begin();

  CompassCalib cal;
  bool calibrated = false;

  // Controlla se esiste un file di calibrazione salvato
  if (InternalFS.exists(CALIB_FILE)) {
    File f(InternalFS);
    f.open(CALIB_FILE, FILE_O_READ);
    if (f.read((uint8_t*)&cal, sizeof(cal)) == (int)sizeof(cal)) {
      calibrated = true;
    }
    f.close();
  }

  if (calibrated) {
    compass.setCalibration(cal.xMin, cal.xMax, cal.yMin, cal.yMax, cal.zMin, cal.zMax);
    Serial.println("Calibrazione bussola caricata da LittleFS.");
  } else {
    // Prima accensione: calibrazione guidata via seriale
    Serial.println("=== CALIBRAZIONE BUSSOLA ===");
    Serial.println("Ruota lentamente il dispositivo su tutti gli assi per 30s...");

    cal.xMin =  32767; cal.xMax = -32768;
    cal.yMin =  32767; cal.yMax = -32768;
    cal.zMin =  32767; cal.zMax = -32768;

    // Sveglia sensori per la calibrazione
    imuWake();
    compass.setMode(0x01, 0x00, 0x00, 0x00);
    delay(10);

    unsigned long tCal = millis();
    while (millis() - tCal < 30000) {
      compass.read();
      int cx = compass.getX();
      int cy = compass.getY();
      int cz = compass.getZ();
      if (cx < cal.xMin) cal.xMin = cx;
      if (cx > cal.xMax) cal.xMax = cx;
      if (cy < cal.yMin) cal.yMin = cy;
      if (cy > cal.yMax) cal.yMax = cy;
      if (cz < cal.zMin) cal.zMin = cz;
      if (cz > cal.zMax) cal.zMax = cz;
      delay(50);
    }

    // Rimuovi file precedente se presente, poi scrivi
    if (InternalFS.exists(CALIB_FILE)) {
      InternalFS.remove(CALIB_FILE);
    }
    File f(InternalFS);
    f.open(CALIB_FILE, FILE_O_WRITE);
    f.write((uint8_t*)&cal, sizeof(cal));
    f.close();

    compass.setCalibration(cal.xMin, cal.xMax, cal.yMin, cal.yMax, cal.zMin, cal.zMax);
    Serial.println("Calibrazione salvata su LittleFS!");
  }
}

// ============================================================
// COMPASS – Leggi heading con tilt compensation
// Aggiorna la variabile globale deviceHeading (0–359°)
// ============================================================
void updateDeviceHeading() {
  // Sveglia sensori
  imuWake();
  compass.setMode(0x01, 0x00, 0x00, 0x00);
  delay(10);

  // Leggi magnetometro
  compass.read();
  float mx = (float)compass.getX();
  float my = (float)compass.getY();
  float mz = (float)compass.getZ();

  // Leggi accelerometro per tilt compensation
  float ax = imu.readFloatAccelX();
  float ay = imu.readFloatAccelY();
  float az = imu.readFloatAccelZ();

  // Calcola roll e pitch dall'accelerometro
  float roll  = atan2(ay, az);
  float pitch = atan2(-ax, sqrtf(ay * ay + az * az));

  // Tilt compensation: proietta il vettore magnetico sul piano orizzontale
  float mx2 =  mx * cosf(pitch)
             + mz * sinf(pitch);

  float my2 =  mx * sinf(roll) * sinf(pitch)
             + my * cosf(roll)
             - mz * sinf(roll) * cosf(pitch);

  // Heading: 0° = Nord, cresce in senso orario
  float heading_rad = atan2f(-my2, mx2);
  int heading = (int)(heading_rad * (180.0f / (float)M_PI));
  if (heading < 0) heading += 360;

  deviceHeading = heading;

  // Rimetti i sensori in sleep
  imuSleep();
  compass.setMode(0x00, 0x00, 0x00, 0x00);
}

// ============================================================
// IMU – Power management
// ============================================================
void imuSleep() {
  // CTRL1_XL = 0x00 → accelerometro power-down
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x00);
  // CTRL2_G  = 0x00 → giroscopio power-down
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL2_G,  0x00);
}

void imuWake() {
  // 0x40 = ODR 104Hz, ±2g
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x40);
  delay(10);
}

// ============================================================
// Direzione vento con compensazione heading
//
// La banderuola misura l'angolo del vento RELATIVO
// all'orientamento fisico del dispositivo.
// Aggiungendo deviceHeading otteniamo la direzione
// assoluta (riferita al Nord magnetico).
//
//   windAbsolute = (vaneRelative + deviceHeading) mod 360
// ============================================================
void getWindDirection() {
  vaneValue     = analogRead(WindVanePin);
  // Angolo grezzo della banderuola: 0–360° relativo al dispositivo
  int vaneAngle = map(vaneValue, 0, 1023, 0, 360);

  // Direzione assoluta = angolo banderuola + heading dispositivo
  windCalDirection = (vaneAngle + deviceHeading) % 360;
  if (windCalDirection < 0) windCalDirection += 360;

  windDirection = windCalDirection; // manteniamo per compatibilità

  // Direzione cardinale
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
  }
}
