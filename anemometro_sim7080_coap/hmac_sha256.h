// ============================================================
// hmac_sha256.h – SHA-256 (FIPS 180-4) e HMAC (RFC 2104).
// Nessuna dipendenza da Arduino.
// ============================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHA256_DIGEST_BYTES 32
#define SHA256_BLOCK_BYTES  64

class Sha256 {
public:
  Sha256() {
    static const uint32_t initial[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(_state, initial, sizeof(_state));
  }

  void update(const uint8_t* data, size_t len) {
    _totalBytes += len;
    while (len > 0) {
      size_t chunk = SHA256_BLOCK_BYTES - _buffered;
      if (chunk > len) chunk = len;
      memcpy(_buffer + _buffered, data, chunk);
      _buffered += chunk;
      data += chunk;
      len -= chunk;
      if (_buffered == SHA256_BLOCK_BYTES) {
        processBlock();
        _buffered = 0;
      }
    }
  }

  void finish(uint8_t digest[SHA256_DIGEST_BYTES]) {
    uint64_t totalBits = _totalBytes * 8;

    _buffer[_buffered++] = 0x80;
    if (_buffered > SHA256_BLOCK_BYTES - 8) {
      memset(_buffer + _buffered, 0, SHA256_BLOCK_BYTES - _buffered);
      processBlock();
      _buffered = 0;
    }
    memset(_buffer + _buffered, 0, SHA256_BLOCK_BYTES - 8 - _buffered);
    for (int i = 0; i < 8; i++) {
      _buffer[SHA256_BLOCK_BYTES - 1 - i] = (uint8_t)(totalBits >> (8 * i));
    }
    processBlock();

    for (int i = 0; i < 8; i++) {
      digest[4 * i]     = (uint8_t)(_state[i] >> 24);
      digest[4 * i + 1] = (uint8_t)(_state[i] >> 16);
      digest[4 * i + 2] = (uint8_t)(_state[i] >> 8);
      digest[4 * i + 3] = (uint8_t)(_state[i]);
    }
  }

private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void processBlock() {
    static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)_buffer[4 * i] << 24) | ((uint32_t)_buffer[4 * i + 1] << 16)
           | ((uint32_t)_buffer[4 * i + 2] << 8) | _buffer[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];
    for (int i = 0; i < 64; i++) {
      uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;
      h = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
    _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
  }

  uint32_t _state[8];
  uint8_t  _buffer[SHA256_BLOCK_BYTES];
  size_t   _buffered   = 0;
  uint64_t _totalBytes = 0;
};

inline void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* message, size_t messageLen,
                       uint8_t digest[SHA256_DIGEST_BYTES]) {
  uint8_t block[SHA256_BLOCK_BYTES] = {0};
  if (keyLen > SHA256_BLOCK_BYTES) {
    Sha256 keyHash;
    keyHash.update(key, keyLen);
    keyHash.finish(block);
  } else {
    memcpy(block, key, keyLen);
  }

  uint8_t pad[SHA256_BLOCK_BYTES];
  for (int i = 0; i < SHA256_BLOCK_BYTES; i++) pad[i] = block[i] ^ 0x36;
  Sha256 inner;
  inner.update(pad, sizeof(pad));
  inner.update(message, messageLen);
  uint8_t innerDigest[SHA256_DIGEST_BYTES];
  inner.finish(innerDigest);

  for (int i = 0; i < SHA256_BLOCK_BYTES; i++) pad[i] = block[i] ^ 0x5c;
  Sha256 outer;
  outer.update(pad, sizeof(pad));
  outer.update(innerDigest, sizeof(innerDigest));
  outer.finish(digest);
}
