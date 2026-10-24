// Host tests for the hardware-independent modules (dsp/, audio/Wav, core/).
// Build + run: python tests/run_host_tests.py
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/Wav.h"
#include "core/RetryPolicy.h"
#include "dsp/GapConcealer.h"
#include "dsp/LungFilter.h"

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

static int32_t i2sWord(double value) {  // value in 16-bit full-scale units -> I2S word
  double scaled = value * 65536;
  if (scaled > 2147483520.0) scaled = 2147483520.0;
  if (scaled < -2147483648.0) scaled = -2147483648.0;
  return int32_t(llrint(scaled));
}

// Output/input amplitude in dB for a sine at `hz` (gain x1, limiter not reached).
static double responseDb(double hz) {
  LungFilterConfig c;
  c.gain = 1.0f;
  LungFilter filter(c);
  const double amp = 1000;
  double in = 0, out = 0;
  for (int i = 0; i < 48000; ++i) {
    double x = amp * sin(2 * kPi * hz * i / 16000.0);
    int16_t y = filter.process(i2sWord(x));
    if (i >= 16000) {  // skip 1 s of settling
      in += x * x;
      out += double(y) * y;
    }
  }
  return 10 * log10(out / in);
}

static void testBandPassResponse() {
  CHECK(responseDb(5) < -40);                 // drift, posture changes
  CHECK(responseDb(10) < -20);
  CHECK(fabs(responseDb(20) + 3) < 1.0);      // -3 dB corner
  CHECK(fabs(responseDb(60)) < 0.5);          // low band ICBHI is full of: kept
  CHECK(fabs(responseDb(300)) < 0.5);         // breath band flat
  CHECK(fabs(responseDb(1000)) < 0.5);
  CHECK(fabs(responseDb(2000) + 3) < 0.6);    // -3 dB corner
  CHECK(responseDb(4000) < -20);              // upper speech, hiss
}

static void testChestLevelDoesNotClip() {
  // Levels measured on the device: raw peaks around -25 dBFS dominated by
  // slow chest-piece pressure, a large INMP441 DC offset, quiet breath on top.
  LungFilter filter;  // defaults: natural scale, gain x2
  AudioStats stats;
  for (int i = 0; i < 48000; ++i) {
    double t = i / 16000.0;
    double x = 3800                                   // DC offset
               + 1200 * sin(2 * kPi * 0.3 * t)        // breathing pressure
               + 400 * sin(2 * kPi * 8 * t)           // handling
               + 300 * sin(2 * kPi * 60 * t)          // heart / low lung sound
               + 30 * sin(2 * kPi * 400 * t);         // breath sound
    int16_t y = filter.process(i2sWord(x));
    if (i == 16000) filter.clearStats();  // like Capture: ignore the warm-up
    if (i >= 16000) stats.add(&y, 1);
  }
  CHECK(filter.limited() == 0);
  CHECK(stats.peak < 2000);   // ~ -24 dBFS, ICBHI-like headroom
  CHECK(stats.peak > 400);    // the 60 Hz band survives (x2 gain)
}

static void testFilterBoundsAndDc() {
  LungFilter filter;
  int16_t peak = 0;
  for (int i = 0; i < 16000; ++i) {
    int32_t word = (i / 8) % 2 ? INT32_MAX : INT32_MIN;  // full-scale 1 kHz square wave
    int16_t y = filter.process(word);
    if (abs(y) > peak) peak = int16_t(abs(y));
  }
  CHECK(peak <= 30000);
  CHECK(filter.limited() > 0);

  filter.reset();
  int16_t last = 0;
  for (int i = 0; i < 48000; ++i) last = filter.process(i2sWord(5000));  // pure DC
  CHECK(abs(last) <= 1);
}

static void testStats() {
  AudioStats stats;
  int16_t pcm[4] = {3, -4, 0, 0};
  stats.add(pcm, 4);
  CHECK(stats.samples == 4);
  CHECK(stats.peak == 4);
  CHECK(fabsf(stats.rms() - 2.5f) < 1e-4f);
}

static void testConcealerRepeatsAndFades() {
  GapConcealer c;
  int16_t out[GapConcealer::kFadeSamples + 100];
  c.fill(out, 10);  // nothing heard yet -> silence
  for (int i = 0; i < 10; ++i) CHECK(out[i] == 0);

  c.reset();
  int16_t block[GapConcealer::kHistory];
  for (size_t i = 0; i < GapConcealer::kHistory; ++i) block[i] = int16_t(1000 + i);
  c.push(block, GapConcealer::kHistory);
  c.fill(out, GapConcealer::kFadeSamples + 100);
  CHECK(out[0] == 1000);                                  // starts with the oldest sample
  CHECK(out[GapConcealer::kHistory] < 1000);              // second repeat is attenuated
  CHECK(out[GapConcealer::kHistory] > 0);
  for (size_t i = 1; i < GapConcealer::kFadeSamples; ++i) {
    size_t prevSrc = (i - 1) % GapConcealer::kHistory, src = i % GapConcealer::kHistory;
    if (src > prevSrc) CHECK(out[i] <= out[i - 1] + 1);   // monotone fade within a ramp
  }
  for (size_t i = GapConcealer::kFadeSamples; i < GapConcealer::kFadeSamples + 100; ++i)
    CHECK(out[i] == 0);
  CHECK(c.concealed() == GapConcealer::kFadeSamples + 100);
  CHECK(c.gaps() == 1);

  // A gap split over two fill() calls continues the same ramp.
  c.reset();
  c.push(block, GapConcealer::kHistory);
  int16_t a[300], b[300];
  c.fill(a, 300);
  c.fill(b, 300);
  CHECK(b[0] == int16_t(int32_t(block[300 % GapConcealer::kHistory]) *
                        int32_t(GapConcealer::kFadeSamples - 300) /
                        int32_t(GapConcealer::kFadeSamples)));
  CHECK(c.gaps() == 1);
  c.push(block, 1);  // good audio ends the gap
  c.fill(a, 1);
  CHECK(c.gaps() == 2);

  // Partial history repeats only what was heard.
  c.reset();
  int16_t two[2] = {7, 9};
  c.push(two, 2);
  c.fill(a, 4);
  CHECK(a[0] == 7 && a[1] == 8 && a[2] == 6 && a[3] == 8);  // 7,9,7,9 with ~0.1% fade
}

static void testWavHeader() {
  uint8_t h[wav::kHeaderBytes];
  const uint32_t pcm = 20 * cfg::kBytesPerSecond;
  wav::buildHeader(h, pcm);
  CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0);
  CHECK(memcmp(h + 36, "data", 4) == 0);
  CHECK((h[4] | h[5] << 8 | h[6] << 16 | uint32_t(h[7]) << 24) == pcm + 36);
  CHECK((h[24] | h[25] << 8) == 16000);
  CHECK(h[34] == 16 && h[22] == 1 && h[20] == 1);
  CHECK(wav::validHeader(h, pcm));
  CHECK(!wav::validHeader(h, pcm - 2));
  CHECK(!wav::validHeader(h, pcm, 8000));
  h[0] = 'X';
  CHECK(!wav::validHeader(h, pcm));
}

static void testRetryPolicy() {
  CHECK(classifyApi(200) == Outcome::Success);
  CHECK(classifyApi(201) == Outcome::Success);
  CHECK(classifyApi(-1) == Outcome::Retry);   // HTTPC_ERROR_CONNECTION_REFUSED
  CHECK(classifyApi(-11) == Outcome::Retry);  // HTTPC_ERROR_READ_TIMEOUT / uploader errors
  CHECK(classifyApi(0) == Outcome::Retry);
  CHECK(classifyApi(408) == Outcome::Retry);
  CHECK(classifyApi(429) == Outcome::Retry);
  CHECK(classifyApi(503) == Outcome::Retry);
  CHECK(classifyApi(401) == Outcome::Fatal);  // wrong device key: retrying cannot fix
  CHECK(classifyApi(404) == Outcome::Fatal);
  CHECK(classifyApi(409) == Outcome::Fatal);
  CHECK(classifyApi(415) == Outcome::Fatal);
  CHECK(classifyStoragePut(400) == Outcome::RefreshUrl);
  CHECK(classifyStoragePut(403) == Outcome::RefreshUrl);
  CHECK(classifyStoragePut(413) == Outcome::Fatal);
  CHECK(classifyStoragePut(-14) == Outcome::Retry);

  CHECK(backoffMs(0, 2000, 30000, 0) == 2000);
  CHECK(backoffMs(1, 2000, 30000, 0) == 4000);
  CHECK(backoffMs(3, 2000, 30000, 0) == 16000);
  CHECK(backoffMs(4, 2000, 30000, 0) == 30000);
  CHECK(backoffMs(40, 2000, 30000, 7) == 30007);  // no shift overflow
  CHECK(backoffMs(31, 60000, 600000, 0) == 600000);
}

int main() {
  testBandPassResponse();
  testChestLevelDoesNotClip();
  testFilterBoundsAndDc();
  testStats();
  testConcealerRepeatsAndFades();
  testWavHeader();
  testRetryPolicy();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("PASS: band-pass response, chest level without clipping, limiter/DC, concealment, WAV header, retry policy\n");
  return 0;
}
