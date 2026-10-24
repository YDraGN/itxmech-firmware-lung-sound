#pragma once
// Producer side of the audio pipeline.
//
//   I2S DMA --> capture task (prio 5) --> StreamBuffer (~1 s) --> consumer
//               filter + loss concealment                         (flash / serial / UDP)
//
// The capture task never touches flash or the network, so a slow LittleFS
// erase or a TLS stall in another task cannot make the DMA overflow. If audio
// is lost anyway, the gap is filled by GapConcealer and counted, so the
// stream keeps an exact sample count.
#include <Arduino.h>
#include <freertos/stream_buffer.h>
#include "dsp/GapConcealer.h"
#include "dsp/LungFilter.h"
#include "hal/I2sMic.h"

struct CaptureStats {
  uint32_t frames = 0;          // samples delivered (good + concealed)
  uint32_t concealed = 0;       // samples synthesised to cover loss
  uint32_t gaps = 0;            // number of loss events
  uint32_t dmaDropped = 0;      // samples the I2S driver discarded
  uint32_t ringDropped = 0;     // samples the consumer was too slow to take
  uint32_t readErrors = 0;
  uint32_t limited = 0;         // samples hitting the soft limiter
  int16_t peak = 0;
  int32_t rawMin = 0;           // raw 32-bit I2S words before the filter
  int32_t rawMax = 0;
  float rms = 0;
};

class Capture {
 public:
  Capture();
  bool begin();
  bool start();   // flush, warm up, start the producer task
  void stop();    // stop the producer and wait for it to exit
  // Blocks up to timeoutMs; returns samples copied into out.
  size_t read(int16_t *out, size_t maxSamples, uint32_t timeoutMs);
  CaptureStats stats() const;
  bool faulted() const { return fault_; }

 private:
  I2sMic mic_;
  LungFilter filter_;
  GapConcealer concealer_;
  AudioStats audio_;
  StreamBufferHandle_t ring_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t exited_ = nullptr;
  volatile bool running_ = false;
  volatile bool fault_ = false;
  bool rawSeen_ = false;
  CaptureStats stats_;

  void release();  // free DMA buffers and the ring
  static void taskEntry(void *self);
  void run();
  void deliver(const int16_t *pcm, size_t n);
  void conceal(uint32_t samples);
};
