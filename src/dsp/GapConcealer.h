#pragma once
// Packet/sample-loss concealment by repetition.
//
// When audio is lost (I2S DMA overflow on the device, a dropped UDP packet on
// the bench link) the gap is filled by repeating the most recent good audio
// with a linear fade-out. The stream keeps its exact length, so the WAV
// duration and timing stay correct and the gap does not click.
// The same algorithm is mirrored in tools/udp_record.py.
#include <stddef.h>
#include <stdint.h>

class GapConcealer {
 public:
  static constexpr size_t kHistory = 512;          // one block, 32 ms
  static constexpr uint32_t kFadeSamples = 2048;   // silent after 128 ms of loss

  void reset();
  void push(const int16_t *pcm, size_t n);  // remember good audio, ends a gap
  void fill(int16_t *out, size_t n);        // produce n concealment samples
  uint32_t concealed() const { return concealed_; }
  uint32_t gaps() const { return gaps_; }

 private:
  int16_t history_[kHistory] = {};
  size_t filled_ = 0;
  size_t head_ = 0;      // next write position in history_
  uint32_t gapPos_ = 0;  // samples produced in the current gap
  bool inGap_ = false;
  uint32_t concealed_ = 0;
  uint32_t gaps_ = 0;
};
