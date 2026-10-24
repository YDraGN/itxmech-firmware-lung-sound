#include "dsp/GapConcealer.h"

void GapConcealer::reset() {
  filled_ = head_ = 0;
  gapPos_ = 0;
  inGap_ = false;
  concealed_ = gaps_ = 0;
}

void GapConcealer::push(const int16_t *pcm, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    history_[head_] = pcm[i];
    head_ = (head_ + 1) % kHistory;
    if (filled_ < kHistory) ++filled_;
  }
  if (n) {
    inGap_ = false;
    gapPos_ = 0;
  }
}

void GapConcealer::fill(int16_t *out, size_t n) {
  if (n && !inGap_) {
    inGap_ = true;
    ++gaps_;
  }
  // Oldest sample of the history window; repeat the window in order.
  size_t start = (head_ + kHistory - filled_) % kHistory;
  for (size_t i = 0; i < n; ++i, ++gapPos_) {
    if (!filled_ || gapPos_ >= kFadeSamples) {
      out[i] = 0;
      continue;
    }
    int32_t source = history_[(start + gapPos_ % filled_) % kHistory];
    int32_t remaining = int32_t(kFadeSamples - gapPos_);
    out[i] = int16_t(source * remaining / int32_t(kFadeSamples));
  }
  concealed_ += n;
}
