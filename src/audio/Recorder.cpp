#include "audio/Recorder.h"
#include <esp_task_wdt.h>
#include "audio/Wav.h"
#include "core/Config.h"

namespace {
constexpr uint32_t kIdleLimitMs = 2000;  // no audio for this long -> fail
constexpr uint32_t kSlowWriteMs = 200;   // log flash writes slower than this
}

bool Recorder::record(File &file, uint32_t seconds, CaptureStats &stats) {
  const uint32_t target = seconds * cfg::kBytesPerSecond;
  // The final size is known, so write the real header first. Patching it at
  // the end would make LittleFS rewrite the whole 640 KB file (~5 s).
  // A short recording is never used: the caller deletes it.
  uint8_t header[wav::kHeaderBytes];
  wav::buildHeader(header, target);
  if (target + wav::kHeaderBytes > cfg::kMaxWavBytes ||
      file.write(header, sizeof(header)) != sizeof(header)) return false;
  if (!capture_.start()) return false;

  static int16_t chunk[cfg::kFlashChunkSamples];
  uint32_t written = 0, lastData = millis(), slowWrites = 0, worstWrite = 0;
  const uint32_t deadline = millis() + seconds * 1000 + 5000;
  bool ok = true;
  while (written < target) {
    esp_task_wdt_reset();
    size_t want = min<uint32_t>(cfg::kFlashChunkSamples, (target - written) / 2);
    size_t got = capture_.read(chunk, want, 100);
    if (!got) {
      if (capture_.faulted() || millis() - lastData > kIdleLimitMs ||
          int32_t(millis() - deadline) > 0) { ok = false; break; }
      continue;
    }
    lastData = millis();
    uint32_t t0 = millis();
    size_t bytes = got * sizeof(int16_t);
    if (file.write(reinterpret_cast<uint8_t *>(chunk), bytes) != bytes) { ok = false; break; }
    uint32_t took = millis() - t0;
    if (took > kSlowWriteMs) ++slowWrites;
    if (took > worstWrite) worstWrite = took;
    written += bytes;
  }
  capture_.stop();
  stats = capture_.stats();

  Serial.printf("[AUDIO] %u/%u bytes, concealed=%u in %u gap(s) (dma=%u ring=%u), "
                "limited=%u, peak=%d, rms=%.0f, raw %ld..%ld, flash worst=%ums slow=%u\n",
                unsigned(written), unsigned(target), unsigned(stats.concealed),
                unsigned(stats.gaps), unsigned(stats.dmaDropped), unsigned(stats.ringDropped),
                unsigned(stats.limited), int(stats.peak), stats.rms, long(stats.rawMin),
                long(stats.rawMax), unsigned(worstWrite), unsigned(slowWrites));
  if (!ok || written != target) return false;
  file.flush();
  return file.size() == target + wav::kHeaderBytes;
}
