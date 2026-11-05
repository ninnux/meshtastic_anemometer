// ============================================================
// bridge_line.h – Riga di comando digitata sul terminale.
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>

// Riga di comando del modem secondo il manuale AT
#define BRIDGE_LINE_MAX 559

enum class LineEvent {
  None,       // carattere memorizzato o ignorato
  Stored,     // carattere memorizzato: va mostrato sul terminale
  Erased,     // ultimo carattere cancellato
  Complete,   // riga pronta in text()
  TooLong,    // riga scartata: inviarla tagliata darebbe un comando sbagliato
};

class LineBuffer {
public:
  LineEvent feed(char c) {
    if (c == '\r' || c == '\n') {
      bool tooLong = _tooLong;
      _tooLong = false;
      _text[_length] = '\0';
      if (tooLong) {
        _length = 0;
        return LineEvent::TooLong;
      }
      return _length > 0 ? LineEvent::Complete : LineEvent::None;  // \n dopo \r o riga vuota
    }
    if (c == '\b' || c == 0x7F) {
      if (_length == 0 || _tooLong) return LineEvent::None;
      _length--;
      return LineEvent::Erased;
    }
    if (c < 0x20 || c > 0x7E) return LineEvent::None;  // un comando AT e' solo testo
    if (_length == BRIDGE_LINE_MAX) {
      _tooLong = true;
      return LineEvent::None;
    }
    if (_tooLong) return LineEvent::None;
    _text[_length++] = c;
    return LineEvent::Stored;
  }

  const char* text() const { return _text; }

  // Da chiamare dopo aver usato la riga
  void clear() { _length = 0; }

private:
  char   _text[BRIDGE_LINE_MAX + 1] = {0};
  size_t _length  = 0;
  bool   _tooLong = false;
};
