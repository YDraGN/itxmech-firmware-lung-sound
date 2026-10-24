#include "dsp/LungFilter.h"
#include <math.h>

namespace {
constexpr float kPi = 3.14159265358979f;
// Q of the two sections of a 4th-order Butterworth filter.
constexpr float kButterQ1 = 0.54119610f;
constexpr float kButterQ2 = 1.30656296f;
}  // namespace

// RBJ audio-EQ-cookbook coefficients, normalised by a0.
void Biquad::highPass(float hz, float fs, float q) {
  float w = 2 * kPi * hz / fs, alpha = sinf(w) / (2 * q), c = cosf(w), a0 = 1 + alpha;
  b0_ = (1 + c) / 2 / a0;
  b1_ = -(1 + c) / a0;
  b2_ = b0_;
  a1_ = -2 * c / a0;
  a2_ = (1 - alpha) / a0;
  reset();
}

void Biquad::lowPass(float hz, float fs, float q) {
  float w = 2 * kPi * hz / fs, alpha = sinf(w) / (2 * q), c = cosf(w), a0 = 1 + alpha;
  b0_ = (1 - c) / 2 / a0;
  b1_ = (1 - c) / a0;
  b2_ = b0_;
  a1_ = -2 * c / a0;
  a2_ = (1 - alpha) / a0;
  reset();
}

LungFilter::LungFilter(const LungFilterConfig &config) : c_(config) {
  stages_[0].highPass(c_.highPassHz, c_.sampleRate, kButterQ1);
  stages_[1].highPass(c_.highPassHz, c_.sampleRate, kButterQ2);
  stages_[2].lowPass(c_.lowPassHz, c_.sampleRate, kButterQ1);
  stages_[3].lowPass(c_.lowPassHz, c_.sampleRate, kButterQ2);
}

void LungFilter::reset() {
  for (Biquad &stage : stages_) stage.reset();
  limited_ = 0;
}

int16_t LungFilter::process(int32_t raw) {
  // The high-pass also removes the INMP441 DC offset.
  // 24-bit audio left-justified in 32 bits converts to float exactly.
  float y = float(raw) * c_.inputScale;
  for (Biquad &stage : stages_) y = stage.process(y);
  y *= c_.gain;

  // Safety limiter: only real overloads (knocks on the chest piece) reach it.
  if (y > c_.knee) {
    y = c_.knee + (y - c_.knee) * c_.kneeSlope;
    ++limited_;
  } else if (y < -c_.knee) {
    y = -c_.knee + (y + c_.knee) * c_.kneeSlope;
    ++limited_;
  }
  if (y > c_.ceiling) y = c_.ceiling;
  if (y < -c_.ceiling) y = -c_.ceiling;
  return int16_t(lrintf(y));
}

void LungFilter::process(const int32_t *raw, int16_t *out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = process(raw[i]);
}

void AudioStats::add(const int16_t *pcm, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    int32_t v = pcm[i];
    int32_t a = v < 0 ? -v : v;
    if (a > peak) peak = int16_t(a > 32767 ? 32767 : a);
    sumSquares += double(v) * v;
  }
  samples += n;
}

float AudioStats::rms() const {
  return samples ? float(sqrt(sumSquares / samples)) : 0.0f;
}
