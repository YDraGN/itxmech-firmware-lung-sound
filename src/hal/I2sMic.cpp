#include "hal/I2sMic.h"
#include "core/Config.h"

namespace {
constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr int kEventQueueLen = 64;
}

bool I2sMic::begin() {
  if (ready_) return true;
  i2s_config_t config = {};
  config.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = cfg::kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // INMP441 L/R pin to GND = left slot, to VDD = right slot. probe() picks
  // whichever slot actually carries audio (also covers driver slot swaps).
  config.channel_format = slot_ == Slot::Left ? I2S_CHANNEL_FMT_ONLY_LEFT
                                              : I2S_CHANNEL_FMT_ONLY_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = cfg::kDmaBufCount;
  config.dma_buf_len = cfg::kBlockFrames;
  config.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = cfg::kPinI2sSck;
  pins.ws_io_num = cfg::kPinI2sWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = cfg::kPinI2sSd;

  esp_err_t err = i2s_driver_install(kPort, &config, kEventQueueLen, &events_);
  if (err == ESP_OK) err = i2s_set_pin(kPort, &pins);
  if (err == ESP_OK) err = i2s_zero_dma_buffer(kPort);
  if (err != ESP_OK) {
    Serial.printf("[I2S] init failed: %s\n", esp_err_to_name(err));
    i2s_driver_uninstall(kPort);
    events_ = nullptr;
    return false;
  }
  ready_ = true;
  return true;
}

void I2sMic::end() {
  if (!ready_) return;
  i2s_driver_uninstall(kPort);  // also deletes the event queue
  events_ = nullptr;
  ready_ = false;
}

uint32_t I2sMic::drainOverflows() {
  uint32_t overflows = 0;
  i2s_event_t event;
  while (events_ && xQueueReceive(events_, &event, 0) == pdTRUE)
    if (event.type == I2S_EVENT_RX_Q_OVF) ++overflows;
  return overflows;
}

int I2sMic::read(int32_t *raw, size_t frames, uint32_t timeoutMs, uint32_t *dropped) {
  size_t bytes = 0;
  esp_err_t err = i2s_read(kPort, raw, frames * sizeof(int32_t), &bytes,
                           pdMS_TO_TICKS(timeoutMs));
  // Each RX queue overflow means the driver dropped its oldest DMA buffer.
  uint32_t overflows = drainOverflows();
  if (dropped) *dropped += overflows * cfg::kBlockFrames;
  if (err != ESP_OK) return -1;
  return int(bytes / sizeof(int32_t));
}

bool I2sMic::probe(Slot slot, ProbeResult &out) {
  out = ProbeResult();
  Slot previous = slot_;
  slot_ = slot;
  bool ok = begin();
  if (ok) {
    int32_t raw[cfg::kBlockFrames];
    // INMP441 outputs zeros until it has seen ~2^18 SCK cycles (~256 ms).
    uint32_t skip = cfg::kSampleRate * kProbeSkipMs / 1000;
    uint32_t measure = cfg::kSampleRate * kProbeMeasureMs / 1000;
    while (skip + measure) {
      int n = read(raw, cfg::kBlockFrames, 100, nullptr);
      if (n <= 0) {
        ok = false;
        break;
      }
      for (int i = 0; i < n; ++i) {
        if (skip) {
          --skip;
          continue;
        }
        if (!measure) break;
        --measure;
        if (!out.samples || raw[i] < out.min) out.min = raw[i];
        if (!out.samples || raw[i] > out.max) out.max = raw[i];
        ++out.samples;
      }
    }
    end();
  }
  slot_ = previous;
  // Mic noise floor alone spans ~1e5 in the 32-bit word; a stuck line spans 0.
  out.alive = ok && out.samples && int64_t(out.max) - out.min > kAliveSpan;
  return ok;
}

void I2sMic::flush() {
  i2s_zero_dma_buffer(kPort);
  int32_t scratch[64];
  size_t bytes = 0;
  // Drain whatever the DMA already holds without blocking.
  while (i2s_read(kPort, scratch, sizeof(scratch), &bytes, 0) == ESP_OK && bytes) {}
  drainOverflows();
}
