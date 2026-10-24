#pragma once
// Lung-sound conditioning chain. Pure C++ so it runs in host tests.
//
//   raw 32-bit I2S word / 65536   (mic full scale = 16-bit full scale)
//   -> 4th-order Butterworth high-pass (default 20 Hz: drift/DC only)
//   -> 4th-order Butterworth low-pass  (default 2000 Hz: room speech/hiss)
//   -> gain -> soft limiter (safety only) -> int16
//
// Targets the ICBHI 2017 recordings the classifier was trained on (Meditron
// chest piece): RMS -24..-32 dBFS, peaks -8..-18 dBFS, no clipping, 89-99% of
// the power below 150 Hz (breathing and heart sounds through the chest piece)
// and almost nothing above 600 Hz. The INMP441 at its natural scale lands at
// that level already. The bring-up sketch (>>11, x3, low-shelf x2.4) added
// about +40 dB: 51% of samples hit the clamp and room speech became audible.
#include <stddef.h>
#include <stdint.h>

struct LungFilterConfig {
  float inputScale = 1.0f / 65536.0f;  // 32-bit I2S word -> 16-bit full scale
  float sampleRate = 16000.0f;
  float highPassHz = 20.0f;     // keep the low band the model learned from
  float lowPassHz = 2000.0f;    // crackles/wheezes stay, most room speech goes
  float gain = 2.0f;            // MIC_GAIN in .env; 1 = natural INMP441 level
  float knee = 24000.0f;        // soft limiter threshold: should rarely engage
  float kneeSlope = 0.1f;
  float ceiling = 30000.0f;
};

struct AudioStats {
  uint32_t samples = 0;
  uint32_t limited = 0;  // samples that went through the soft limiter
  int16_t peak = 0;
  double sumSquares = 0;
  float rms() const;
  void add(const int16_t *pcm, size_t n);
};

class Biquad {
 public:
  void highPass(float hz, float fs, float q);
  void lowPass(float hz, float fs, float q);
  void reset() { z1_ = z2_ = 0; }
  float process(float x) {
    // Transposed direct form II: good numeric behaviour in float.
    float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }

 private:
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float z1_ = 0, z2_ = 0;
};

class LungFilter {
 public:
  explicit LungFilter(const LungFilterConfig &config = LungFilterConfig());
  void reset();
  int16_t process(int32_t raw);
  void process(const int32_t *raw, int16_t *out, size_t n);
  uint32_t limited() const { return limited_; }
  // Forget the warm-up transient (filters settling on the DC offset) without
  // touching the filter state.
  void clearStats() { limited_ = 0; }
  const LungFilterConfig &config() const { return c_; }

 private:
  LungFilterConfig c_;
  Biquad stages_[4];  // HP, HP, LP, LP
  uint32_t limited_ = 0;
};
