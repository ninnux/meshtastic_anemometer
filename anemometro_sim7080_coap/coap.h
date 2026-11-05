// ============================================================
// coap.h – Codifica di una richiesta CoAP POST non confermabile
// (RFC 7252). Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define COAP_CONTENT_FORMAT_JSON 50

// Scrive in out il datagramma per POST su path ("wind/nome").
// Ritorna la lunghezza, oppure 0 se non entra in cap o se il path
// contiene un segmento vuoto o troppo lungo.
inline size_t coapBuildNonPost(uint8_t* out, size_t cap, uint16_t messageId,
                               const char* path, uint8_t contentFormat,
                               const char* payload, size_t payloadLen) {
  const uint8_t COAP_V1_NON_TKL0 = 0x50;  // Ver=1, Type=NON, token vuoto
  const uint8_t COAP_CODE_POST   = 0x02;
  const uint8_t OPT_URI_PATH     = 11;
  const uint8_t OPT_CONTENT_FMT  = 12;

  if (cap < 4) return 0;
  size_t n = 0;
  out[n++] = COAP_V1_NON_TKL0;
  out[n++] = COAP_CODE_POST;
  out[n++] = messageId >> 8;
  out[n++] = messageId & 0xFF;

  // Le opzioni sono codificate come differenza dal numero precedente
  uint8_t delta = OPT_URI_PATH;
  const char* seg = path;
  while (true) {
    const char* slash = strchr(seg, '/');
    size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
    if (len == 0 || len > 268) return 0;

    if (len < 13) {
      if (n + 1 + len > cap) return 0;
      out[n++] = (delta << 4) | len;
    } else {
      if (n + 2 + len > cap) return 0;
      out[n++] = (delta << 4) | 13;
      out[n++] = len - 13;
    }
    memcpy(out + n, seg, len);
    n += len;
    delta = 0;

    if (!slash) break;
    seg = slash + 1;
  }

  if (n + 2 > cap) return 0;
  out[n++] = ((OPT_CONTENT_FMT - OPT_URI_PATH) << 4) | 1;
  out[n++] = contentFormat;

  if (payloadLen > 0) {
    if (n + 1 + payloadLen > cap) return 0;
    out[n++] = 0xFF;
    memcpy(out + n, payload, payloadLen);
    n += payloadLen;
  }
  return n;
}
