// Messaggi di debug sulla USB che non bloccano mai il firmware.
//
// Serial.write della XIAO aspetta finche' il computer collegato non legge:
// con il Mac in stop il buffer si riempie e tutto il firmware si ferma,
// invio compreso. Log scrive a pezzi quanto entra nel buffer (256 byte),
// aspetta al massimo LOG_WAIT_MS che si liberi e poi scarta il resto.
// Dopo uno scarto non aspetta piu' finche' il computer non torna a leggere.
#pragma once
#include <Arduino.h>

#ifndef FAKE_ARDUINO  // nei test sul computer Log e' finto (test/Arduino.h)
#define LOG_WAIT_MS 20

class LogSerial : public Print {
public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    uint32_t start = millis();
    size_t done = 0;
    while (done < len) {
      int room = Serial.availableForWrite();
      if (room > 0) {
        _stalled = false;
        size_t n = len - done < (size_t)room ? len - done : (size_t)room;
        Serial.write(data + done, n);
        done += n;
      } else if (_stalled || millis() - start >= LOG_WAIT_MS) {
        _stalled = true;  // il computer non legge: scarta senza aspettare
        break;
      } else {
        yield();
      }
    }
    return len;  // scritto o scartato, per chi chiama e' andato
  }

private:
  bool _stalled = false;
};
extern LogSerial Log;
#endif
