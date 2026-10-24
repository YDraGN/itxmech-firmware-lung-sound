#include "audio/Capture.h"
#include <BuildConfig.h>
#include "core/Config.h"

namespace {
constexpr uint32_t kReadTimeoutMs = 100;
constexpr uint32_t kMaxConsecutiveErrors = 20;  // ~2 s without I2S data
constexpr TickType_t kRingSendWait = pdMS_TO_TICKS(50);

LungFilterConfig filterConfig() {
  LungFilterConfig c;
  c.sampleRate = cfg::kSampleRate;
  c.gain = MIC_GAIN;
  c.highPassHz = MIC_HPF_HZ;
  c.lowPassHz = MIC_LPF_HZ;
  return c;
}
}

Capture::Capture() : filter_(filterConfig()) {}

// Boot self-test only: the DMA buffers and the ring (~64 KB together) are
// allocated per recording so the heap is free again for TLS uploads.
bool Capture::begin() {
  if (!exited_) exited_ = xSemaphoreCreateBinary();
  if (!exited_) return false;
  I2sMic::ProbeResult left, right;
  if (!mic_.probe(I2sMic::Slot::Left, left) || !mic_.probe(I2sMic::Slot::Right, right))
    return false;
  Serial.printf("[I2S] self-test raw LEFT  %ld..%ld (%s)\n", long(left.min), long(left.max),
                left.alive ? "signal" : "constant");
  Serial.printf("[I2S] self-test raw RIGHT %ld..%ld (%s)\n", long(right.min), long(right.max),
                right.alive ? "signal" : "constant");
  bool useRight = right.alive && (!left.alive ||
                  int64_t(right.max) - right.min > int64_t(left.max) - left.min);
  mic_.setSlot(useRight ? I2sMic::Slot::Right : I2sMic::Slot::Left);
  Serial.printf("[DSP] band-pass %.0f-%.0f Hz (4th-order Butterworth), gain x%.2f over natural mic level\n",
                filter_.config().highPassHz, filter_.config().lowPassHz, filter_.config().gain);
  if (left.alive || right.alive) {
    Serial.printf("[I2S] INMP441 OK on %s slot: %u Hz, DMA %d x %u (allocated while recording)\n",
                  useRight ? "RIGHT" : "LEFT", unsigned(cfg::kSampleRate), cfg::kDmaBufCount,
                  unsigned(cfg::kBlockFrames));
  } else {
    int32_t stuck = left.max;
    Serial.println("[I2S] NO MIC SIGNAL on either slot: recordings will be dropped as silence");
    if (stuck == 0)
      Serial.println("[I2S]   data line reads 0: SD not on GPIO34, mic unpowered (VDD 3.3V/GND), "
                     "or SCK/WS not reaching the mic (GPIO18/19)");
    else if (stuck == -1)
      Serial.println("[I2S]   data line stuck high: SD shorted to 3.3V or wrong pin");
    else
      Serial.println("[I2S]   data constant: check SD wiring and the mic module");
  }
  return true;  // keep running so MQTT/web still work and the cause stays visible
}

void Capture::release() {
  mic_.end();
  if (ring_) vStreamBufferDelete(ring_);
  ring_ = nullptr;
}

bool Capture::start() {
  if (task_) return false;
  ring_ = xStreamBufferCreate(cfg::kRingBytes, sizeof(int16_t));
  if (!ring_ || !mic_.begin()) {
    Serial.printf("[CAPTURE] cannot allocate audio buffers (heap %u, block %u)\n",
                  unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()));
    release();
    return false;
  }
  filter_.reset();
  concealer_.reset();
  audio_ = AudioStats();
  stats_ = CaptureStats();
  rawSeen_ = false;
  fault_ = false;
  mic_.flush();

  // Warm-up: INMP441 start-up and filter settling, discarded.
  int32_t raw[cfg::kBlockFrames];
  int16_t pcm[cfg::kBlockFrames];
  uint32_t warmup = cfg::kSampleRate * cfg::kWarmupMs / 1000;
  while (warmup) {
    int n = mic_.read(raw, cfg::kBlockFrames, kReadTimeoutMs, nullptr);
    if (n <= 0) {
      Serial.println("[CAPTURE] no I2S data during warm-up");
      release();
      return false;
    }
    filter_.process(raw, pcm, n);
    warmup -= min<uint32_t>(warmup, n);
  }
  filter_.clearStats();

  running_ = true;
  xSemaphoreTake(exited_, 0);
  // Core 1, above the Arduino loop and the worker; Wi-Fi lives on core 0.
  if (xTaskCreatePinnedToCore(taskEntry, "capture", 8192, this, 5, &task_, 1) != pdPASS) {
    running_ = false;
    task_ = nullptr;
    release();
    return false;
  }
  return true;
}

void Capture::stop() {
  if (!task_) return;
  running_ = false;
  if (xSemaphoreTake(exited_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[CAPTURE] task did not exit in time; deleting");
    vTaskDelete(task_);
  }
  task_ = nullptr;
  stats_.limited = filter_.limited();
  stats_.peak = audio_.peak;
  stats_.rms = audio_.rms();
  release();
}

size_t Capture::read(int16_t *out, size_t maxSamples, uint32_t timeoutMs) {
  if (!ring_) return 0;
  size_t bytes = xStreamBufferReceive(ring_, out, maxSamples * sizeof(int16_t),
                                      pdMS_TO_TICKS(timeoutMs));
  return bytes / sizeof(int16_t);
}

CaptureStats Capture::stats() const { return stats_; }

void Capture::taskEntry(void *self) {
  static_cast<Capture *>(self)->run();
  xSemaphoreGive(static_cast<Capture *>(self)->exited_);
  vTaskDelete(nullptr);
}

void Capture::deliver(const int16_t *pcm, size_t n) {
  size_t bytes = n * sizeof(int16_t);
  size_t sent = xStreamBufferSend(ring_, pcm, bytes, kRingSendWait);
  stats_.frames += sent / sizeof(int16_t);
  if (sent < bytes) {
    // Consumer stalled for longer than the ring: account for it as loss and
    // conceal it on the next pass so the timeline stays exact.
    stats_.ringDropped += (bytes - sent) / sizeof(int16_t);
  }
}

void Capture::conceal(uint32_t samples) {
  int16_t pcm[cfg::kBlockFrames];
  while (samples) {
    size_t n = min<uint32_t>(samples, cfg::kBlockFrames);
    concealer_.fill(pcm, n);
    deliver(pcm, n);
    samples -= n;
  }
}

void Capture::run() {
  int32_t raw[cfg::kBlockFrames];
  int16_t pcm[cfg::kBlockFrames];
  uint32_t errors = 0;
  uint32_t concealedRing = 0;
  while (running_) {
    uint32_t dropped = 0;
    int n = mic_.read(raw, cfg::kBlockFrames, kReadTimeoutMs, &dropped);
    if (dropped) {
      stats_.dmaDropped += dropped;
      conceal(dropped);
    }
    // Conceal what the consumer could not take last time (bounded to 1 s).
    if (stats_.ringDropped > concealedRing) {
      uint32_t pending = min<uint32_t>(stats_.ringDropped - concealedRing, cfg::kSampleRate);
      concealedRing = stats_.ringDropped;
      conceal(pending);
    }
    if (n <= 0) {
      ++stats_.readErrors;
      if (++errors >= kMaxConsecutiveErrors) {
        Serial.println("[CAPTURE] I2S stopped delivering data");
        fault_ = true;
        break;
      }
      continue;
    }
    errors = 0;
    for (int i = 0; i < n; ++i) {
      if (!rawSeen_ || raw[i] < stats_.rawMin) stats_.rawMin = raw[i];
      if (!rawSeen_ || raw[i] > stats_.rawMax) stats_.rawMax = raw[i];
      rawSeen_ = true;
    }
    filter_.process(raw, pcm, n);
    audio_.add(pcm, n);
    concealer_.push(pcm, n);
    deliver(pcm, n);
  }
  stats_.concealed = concealer_.concealed();
  stats_.gaps = concealer_.gaps();
  running_ = false;
}
