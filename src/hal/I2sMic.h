#pragma once
// INMP441 over the legacy ESP32 I2S driver (Arduino-ESP32 2.x / IDF 4.4).
#include <Arduino.h>
#include <driver/i2s.h>

class I2sMic {
 public:
  enum class Slot : uint8_t { Left, Right };

  struct ProbeResult {
    int32_t min = 0;
    int32_t max = 0;
    uint32_t samples = 0;
    bool alive = false;  // the raw word actually varies (mic noise/audio present)
  };

  bool begin();
  // Uninstalls the driver and frees its DMA buffers (~32 KB) between recordings.
  void end();
  // Reads one I2S slot for a moment and reports its raw range. Leaves the
  // driver uninstalled and the selected slot unchanged.
  bool probe(Slot slot, ProbeResult &out);
  void setSlot(Slot slot) { slot_ = slot; }
  Slot slot() const { return slot_; }
  // Reads up to `frames` 32-bit samples. Returns frames read (0 on timeout,
  // -1 on driver error). Frames the driver discarded because nobody read
  // them in time (DMA queue overflow) are added to *dropped.
  int read(int32_t *raw, size_t frames, uint32_t timeoutMs, uint32_t *dropped);
  // Discard stale DMA audio and pending driver events before a new capture.
  void flush();

 private:
  static constexpr uint32_t kProbeSkipMs = 400;
  static constexpr uint32_t kProbeMeasureMs = 200;
  static constexpr int64_t kAliveSpan = 1 << 14;

  QueueHandle_t events_ = nullptr;
  bool ready_ = false;
  Slot slot_ = Slot::Left;
  uint32_t drainOverflows();
};
