// Sostituto minimo di Arduino.h per eseguire i test sul computer
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#define FAKE_ARDUINO
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1

extern uint32_t fakeMillis;
extern int pwrkeyPulses;
extern uint8_t pwrkeyLevel;
extern uint32_t fakeTickOffset;
inline uint32_t millis() { return fakeMillis; }
#define WIND_FAKE_TICK_HZ 1024
inline uint32_t xTaskGetTickCount() { return (uint32_t)((uint64_t)fakeMillis * WIND_FAKE_TICK_HZ / 1000) + fakeTickOffset; }
inline uint32_t ms2tick(uint32_t ms) { return (uint32_t)((uint64_t)ms * WIND_FAKE_TICK_HZ / 1000); }
void delay(uint32_t ms);
inline void pinMode(int, int) {}
void digitalWrite(int, int level);

class Stream {
public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(const uint8_t* data, size_t len) = 0;
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
};

struct FakeSerial {
  void print(const char*) {}
  void println(const char*) {}
};
extern FakeSerial Serial;
extern FakeSerial Log;
