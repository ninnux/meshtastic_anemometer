// ============================================================
// meshtastic_davis_anemometer.ino
// Board: Seeed XIAO nRF52840 Sense
// BSP:   Seeed nRF52 Boards >= 1.1.0
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

using namespace Adafruit_LittleFS_Namespace;

#define CALIB_FILE "/compass_calib.bin"

struct CompassCalib {
  int xMin, xMax, yMin, yMax, zMin, zMax;
};

#define WindSensorPin  2
#define WindVanePin    A3
#define DEST_NODE      0x12ab2251
#define SERIAL_RX_PIN  7
#define SERIAL_TX_PIN  6
#define BAUD_RATE      38400
#define NODE_NAME      "first_anemometer"
#define ADV_WINDOW_MS  10000UL
#define ADV_PERIOD_MS  30000UL

BLEService        windService("A0010000-0001-0001-0001-000000000001");
BLECharacteristic bleWindData("A0010000-0001-0001-0001-000000000002",
                               CHR_PROPS_READ | CHR_PROPS_NOTIFY, 32);

uint32_t lastAdvStart  = 0;
bool     isAdvertising = false;

SoftwareTimer sampleTimer;

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

QMC5883LCompass compass;
LSM6DS3         imu(I2C_MODE, 0x6A);
int             deviceHeading = 0;

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

pinMode(24, OUTPUT);
digitalWrite(24, HIGH);
delay(10);

  Wire.begin();
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, BAUD_RATE);

  pinMode(WindSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(WindSensorPin), isr_rotation, FALLING);

  setupBLE();
  setupCompass();

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

  if (!Bluefruit.connected()) {
    if (!isAdvertising && (now - lastAdvStart >= ADV_PERIOD_MS)) {
      lastAdvStart = now;
      startAdvertising();
    }
    if (isAdvertising && (now - lastAdvStart >= ADV_WINDOW_MS)) {
      stopAdvertising();
    }
  }

  while (!IsSampleRequired) {
    __WFE();
    __SEV();
    __WFE();
  }
  IsSampleRequired = false;
  Serial.println("DEBUG: sample tick");

  updateDeviceHeading();
  Serial.print("DEBUG: heading="); Serial.println(deviceHeading);

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
  Serial.print(") | Heading: ");     Serial.print(deviceHeading);
  Serial.println("°");

  if (IsBLEUpdateRequired) {
    IsBLEUpdateRequired = false;
    if (Bluefruit.connected()) updateBLE();
  }

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
  InternalFS.remove(CALIB_FILE); ///da rimuovere

  // ── Scan I2C: verifica che QMC5883L (0x0D) e LSM6DS3 (0x6A) rispondano
  Serial.println("=== I2C SCAN ===");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  Trovato dispositivo a 0x");
      Serial.println(addr, HEX);
    }
  }
  Serial.println("=== FINE SCAN ===");

  CompassCalib cal;
  bool calibrated = false;

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
    Serial.println("=== CALIBRAZIONE BUSSOLA ===");
    Serial.println("Ruota lentamente il dispositivo su tutti gli assi per 30s...");

    cal.xMin =  32767; cal.xMax = -32768;
    cal.yMin =  32767; cal.yMax = -32768;
    cal.zMin =  32767; cal.zMax = -32768;

    imuWake();
    compass.setMode(0x01, 0x00, 0x00, 0x00);
    delay(25);

    unsigned long tCal = millis();
    while (millis() - tCal < 30000) {
      // Aspetta DRDY
      uint32_t dw = millis();
      while (millis() - dw < 100) {
        Wire.beginTransmission(0x0D);
        Wire.write(0x06);
        Wire.endTransmission(false);
        Wire.requestFrom(0x0D, 1);
        if (Wire.available() && (Wire.read() & 0x01)) break;
        delay(5);
      }
      compass.read();
      int cx = compass.getX();
      int cy = compass.getY();
      int cz = compass.getZ();
      Serial.println("Dati da calibrare: ");
      Serial.print(cx);
      Serial.print(" ");   
      Serial.print(cy);
      Serial.print(" ");
      Serial.println(cz);
      
      if (cx < cal.xMin) cal.xMin = cx;
      if (cx > cal.xMax) cal.xMax = cx;
      if (cy < cal.yMin) cal.yMin = cy;
      if (cy > cal.yMax) cal.yMax = cy;
      if (cz < cal.zMin) cal.zMin = cz;
      if (cz > cal.zMax) cal.zMax = cz;
      delay(50);
    }

    if (InternalFS.exists(CALIB_FILE)) InternalFS.remove(CALIB_FILE);
    File f(InternalFS);
    f.open(CALIB_FILE, FILE_O_WRITE);
    f.write((uint8_t*)&cal, sizeof(cal));
    f.close();

    compass.setCalibration(cal.xMin, cal.xMax, cal.yMin, cal.yMax, cal.zMin, cal.zMax);
    Serial.println("Calibrazione salvata su LittleFS!");

    imuSleep();
    compass.setMode(0x00, 0x00, 0x00, 0x00);
  }
}

// ============================================================
// COMPASS – Heading con tilt compensation + debug
// ============================================================
void updateDeviceHeading() {
  Serial.println("DEBUG HDG: imuWake");
  imuWake();
  Serial.println("DEBUG HDG: setMode");
  compass.setMode(0x01, 0x00, 0x00, 0x00);

  // Attendi Data Ready (registro 0x06, bit 0 = DRDY)
  // Timeout 200ms per sicurezza
  Serial.println("DEBUG HDG: waiting DRDY");
  uint32_t drdy_t = millis();
  while (millis() - drdy_t < 200) {
    Wire.beginTransmission(0x0D); // indirizzo QMC5883L
    Wire.write(0x06);             // registro STATUS
    Wire.endTransmission(false);
    Wire.requestFrom(0x0D, 1);
    if (Wire.available() && (Wire.read() & 0x01)) break; // DRDY set
    delay(5);
  }
  Serial.println("DEBUG HDG: compass.read");
  compass.read();
  Serial.println("DEBUG HDG: getXYZ");
  float mx = (float)compass.getX();
  float my = (float)compass.getY();
  float mz = (float)compass.getZ();
  Serial.print("DEBUG HDG: mx="); Serial.print(mx);
  Serial.print(" my="); Serial.print(my);
  Serial.print(" mz="); Serial.println(mz);

  Serial.println("DEBUG HDG: readAccel");
  float ax = imu.readFloatAccelX();
  float ay = imu.readFloatAccelY();
  float az = imu.readFloatAccelZ();
  Serial.print("DEBUG HDG: ax="); Serial.print(ax);
  Serial.print(" ay="); Serial.print(ay);
  Serial.print(" az="); Serial.println(az);

  float roll  = atan2(ay, az);
  float pitch = atan2(-ax, sqrtf(ay * ay + az * az));

  float mx2 =  mx * cosf(pitch) + mz * sinf(pitch);
  float my2 =  mx * sinf(roll) * sinf(pitch)
             + my * cosf(roll)
             - mz * sinf(roll) * cosf(pitch);

  float heading_rad = atan2f(-my2, mx2);
  int heading = (int)(heading_rad * (180.0f / (float)M_PI));
  if (heading < 0) heading += 360;

  deviceHeading = heading;

  imuSleep();
  compass.setMode(0x00, 0x00, 0x00, 0x00);
}

// ============================================================
// IMU – Power management
// ============================================================
void imuSleep() {
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x00);
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL2_G,  0x00);
}

void imuWake() {
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x40);
  delay(10);
}

// ============================================================
// Direzione vento con compensazione heading
// ============================================================
void getWindDirection() {
  vaneValue = analogRead(WindVanePin);
  int vaneAngle = map(vaneValue, 0, 1023, 0, 360);
  windCalDirection = (vaneAngle + deviceHeading) % 360;
  if (windCalDirection < 0) windCalDirection += 360;
  windDirection = windCalDirection;

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
// BLE
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

void startAdvertising() {
  if (!isAdvertising) {
    Bluefruit.Advertising.start(0);
    isAdvertising = true;
    Serial.println("BLE advertising ON");
  }
}

void stopAdvertising() {
  if (isAdvertising && !Bluefruit.connected()) {
    Bluefruit.Advertising.stop();
    isAdvertising = false;
    Serial.println("BLE advertising OFF");
  }
}

void updateBLE() {
  char buf[32];
  snprintf(buf, sizeof(buf), "S:%.1f M:%.1f X:%.1f D:%d %s",
           WindSpeed, WindSpeedMean, WindSpeedMax,
           windCalDirection, windCompassDirection);
  bleWindData.write(buf, strlen(buf));
  if (Bluefruit.connected()) bleWindData.notify(buf, strlen(buf));
}

// ============================================================
// ISR
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

void isr_rotation() {
  uint32_t now = millis();
  if ((now - ContactBounceTime) > 15) {
    Rotations++;
    ContactBounceTime = now;
  }
}

