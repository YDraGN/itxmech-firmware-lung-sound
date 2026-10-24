#include "audio/Wav.h"
#include <string.h>

namespace {
void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
}  // namespace

namespace wav {

void buildHeader(uint8_t h[kHeaderBytes], uint32_t pcmBytes, uint32_t sampleRate) {
  const uint16_t blockAlign = cfg::kChannels * cfg::kBitsPerSample / 8;
  memset(h, 0, kHeaderBytes);
  memcpy(h, "RIFF", 4);
  put32(h + 4, pcmBytes + 36);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, cfg::kChannels);
  put32(h + 24, sampleRate);
  put32(h + 28, sampleRate * blockAlign);
  put16(h + 32, blockAlign);
  put16(h + 34, cfg::kBitsPerSample);
  memcpy(h + 36, "data", 4);
  put32(h + 40, pcmBytes);
}

bool validHeader(const uint8_t h[kHeaderBytes], uint32_t pcmBytes, uint32_t sampleRate) {
  uint8_t expected[kHeaderBytes];
  buildHeader(expected, pcmBytes, sampleRate);
  return memcmp(h, expected, kHeaderBytes) == 0;
}

}  // namespace wav
