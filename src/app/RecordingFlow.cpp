#include "app/RecordingFlow.h"
#include <BuildConfig.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "audio/Wav.h"
#include "core/Config.h"

namespace {
constexpr uint32_t kValidEpoch = 1735689600;  // 2025-01-01, NTP sanity check

const char *outcomeName(Outcome o) {
  switch (o) {
    case Outcome::Success: return "ok";
    case Outcome::Retry: return "retry";
    case Outcome::RefreshUrl: return "refresh-url";
    default: return "fatal";
  }
}
}  // namespace

uint32_t RecordingFlow::pcmBytes() { return RECORD_SECONDS * cfg::kBytesPerSecond; }

void RecordingFlow::sleepFed(uint32_t ms) {
  while (ms) {
    uint32_t step = min<uint32_t>(ms, 1000);
    delay(step);
    esp_task_wdt_reset();
    ms -= step;
  }
}

bool RecordingFlow::waitNetwork(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!networkReady_()) {
    if (millis() - start >= timeoutMs) return false;
    sleepFed(250);
  }
  return true;
}

bool RecordingFlow::begin() {
  if (!store_.begin()) {
    Serial.println("[BOOT] NVS unavailable");
    return false;
  }
  Job stored;
  bool hasJob = store_.load(stored);
  if (!slot_.begin(hasJob)) return false;
  if (!recorder_.begin()) {
    Serial.println("[BOOT] I2S microphone init failed");
    return false;
  }
  return true;
}

bool RecordingFlow::checkpoint(Phase phase) {
  job_.phase = phase;
  if (store_.save(job_)) return true;
  Serial.println("[FLOW] checkpoint write failed");
  return false;
}

void RecordingFlow::drop(const char *reason, bool notify) {
  Serial.printf("[FLOW] dropping visit %s: %s\n", job_.visitId.c_str(), reason);
  if (notify && !job_.visitId.isEmpty()) emit_("UPLOAD_FAILED", job_.visitId);
  if (!slot_.remove()) Serial.println("[FLOW] WAV delete failed; slot will be overwritten");
  store_.clear();
  job_ = Job();
  resumeDelivery_ = false;
}

void RecordingFlow::resume() {
  if (!store_.load(job_)) {
    job_ = Job();
    if (slot_.exists()) {
      // No checkpoint references it, so it can never be delivered.
      Serial.println("[FLOW] orphan WAV without checkpoint removed");
      slot_.remove();
    }
    Serial.println("[FLOW] idle");
    return;
  }
  Serial.printf("[FLOW] restoring visit %s phase=%s pending=%d\n", job_.visitId.c_str(),
                phaseName(job_.phase), int(job_.pending));
  if (job_.phase == Phase::Assigned || job_.phase == Phase::Recording) {
    // Capture cannot be resumed; never upload a truncated or re-recorded file.
    drop("reboot before the recording finished", true);
    return;
  }
  resumeDelivery_ = true;  // delivered from service() once the network is up
  nextRetryAt_ = millis();
}

void RecordingFlow::assign(const String &messageId, const String &visitId) {
  if (store_.seenMessage(messageId)) {
    Serial.println("[FLOW] duplicate ASSIGN_VISIT ignored");
    return;
  }
  store_.rememberMessage(messageId);
  if (!idle()) {
    Serial.println("[FLOW] busy; assignment rejected");
    return;
  }
  job_ = Job();
  job_.messageId = messageId;
  job_.visitId = visitId;
  if (!checkpoint(Phase::Assigned)) {
    drop("cannot persist the assignment", true);
    return;
  }
  Serial.printf("[VISIT] %s assigned\n", visitId.c_str());
  emit_("VISIT_ASSIGNED", visitId);
  if (record()) deliver();
}

bool RecordingFlow::record() {
  if (!slot_.remove() || !slot_.hasRoom(pcmBytes() + wav::kHeaderBytes)) {
    drop("not enough flash for the WAV", true);
    return false;
  }
  if (!checkpoint(Phase::Recording)) {
    drop("cannot persist the recording phase", true);
    return false;
  }
  File file = slot_.create();
  if (!file) {
    drop("cannot create the WAV file", true);
    return false;
  }
  emit_("RECORDING_STARTED", job_.visitId);
  Serial.printf("[AUDIO] recording %u s\n", unsigned(RECORD_SECONDS));
  CaptureStats stats;
  bool ok = recorder_.record(file, RECORD_SECONDS, stats);
  file.close();
  if (!ok) {
    drop("audio capture failed", true);
    return false;
  }
  if (stats.peak == 0) {
    // All-zero audio: SD line floating or mic unpowered. Uploading it is useless.
    drop("microphone delivered silence (raw I2S constant; see [I2S] self-test at boot)", true);
    return false;
  }
  if (stats.concealed * 20 > stats.frames)
    Serial.println("[AUDIO] WARNING: >5% of the recording was concealed loss");
  // Level guidance for tuning MIC_GAIN in .env (classifier needs clean, unclipped audio).
  if (stats.limited * 200 > stats.frames)
    Serial.printf("[AUDIO] WARNING: %.1f%% of samples overloaded; lower MIC_GAIN or check chest-piece contact\n",
                  100.0f * stats.limited / stats.frames);
  else if (stats.rms < 150)
    Serial.printf("[AUDIO] NOTE: quiet recording (rms %.0f); raise MIC_GAIN if breath is hard to hear\n",
                  stats.rms);
  if (!checkpoint(Phase::Recorded)) {
    drop("cannot persist the recorded phase", true);
    return false;
  }
  emit_("RECORDING_STOPPED", job_.visitId);
  return true;
}

Outcome RecordingFlow::uploadStep(bool &announced) {
  UploadTicket ticket;
  int code = api_.requestUploadUrl(job_, ticket);
  if (code == RecordingApi::kAlreadyUploaded) {
    Serial.println("[API] Storage already has this recording; completing");
    return checkpoint(Phase::Uploaded) ? Outcome::Success : Outcome::Retry;
  }
  Outcome outcome = classifyApi(code);
  Serial.printf("[API] upload-url -> %d (%s)\n", code, outcomeName(outcome));
  if (outcome != Outcome::Success) return outcome;
  // Persist the backend recording ID before any byte reaches Storage.
  if (!checkpoint(Phase::Uploading)) return Outcome::Retry;
  if (!announced) {
    emit_("UPLOAD_STARTED", job_.visitId);
    announced = true;
  }

  File file = slot_.open();
  UploadReport r;
  code = api_.upload(ticket, file, r);
  file.close();
  outcome = classifyStoragePut(code);
  float kbps = r.bodyMs ? r.bytesSent / 1.024f / r.bodyMs : 0;
  Serial.printf("[UPLOAD] -> %d %s (%s): %u B, connect %u ms, body %u ms (%.1f KB/s), "
                "response %u ms, heap %u/%u, rssi %d\n",
                code, code < 0 ? HttpUploader::errorName(code) : "", outcomeName(outcome),
                unsigned(r.bytesSent), unsigned(r.connectMs), unsigned(r.bodyMs), kbps,
                unsigned(r.responseMs), unsigned(r.freeHeap), unsigned(r.maxBlock), r.rssi);
  if (r.error.length()) Serial.printf("[UPLOAD] detail: %s\n", r.error.c_str());
  if (outcome == Outcome::Success && !checkpoint(Phase::Uploaded)) return Outcome::Retry;
  return outcome;
}

Outcome RecordingFlow::completeStep() {
  int code = api_.complete(job_, pcmBytes() + wav::kHeaderBytes);
  if (code == 404) {
    // Storage lost the object (or the PUT never landed): upload again.
    Serial.println("[API] complete -> 404; re-uploading");
    checkpoint(Phase::Recorded);
    return Outcome::Retry;
  }
  Outcome outcome = classifyApi(code);
  Serial.printf("[API] complete -> %d (%s)\n", code, outcomeName(outcome));
  return outcome;
}

RecordingFlow::Delivery RecordingFlow::goPending() {
  if (!job_.pending) {
    job_.pending = true;
    time_t now = time(nullptr);
    job_.pendingSince = now > kValidEpoch ? uint32_t(now) : 0;
    emit_("UPLOAD_FAILED", job_.visitId);
  }
  store_.save(job_);
  uint32_t delayMs = backoffMs(job_.sessions ? job_.sessions - 1 : 0, cfg::kPendingRetryMinMs,
                               cfg::kPendingRetryMaxMs, esp_random() % 5000);
  nextRetryAt_ = millis() + delayMs;
  Serial.printf("[FLOW] upload pending; WAV kept, next try in %u s\n", unsigned(delayMs / 1000));
  return Delivery::Pending;
}

RecordingFlow::Delivery RecordingFlow::deliver() {
  resumeDelivery_ = false;
  if (!slot_.valid(pcmBytes())) {
    drop("local WAV missing or corrupt", true);
    return Delivery::Dropped;
  }
  ++job_.sessions;
  store_.save(job_);
  Serial.printf("[FLOW] delivery session %u for visit %s (phase=%s)\n", unsigned(job_.sessions),
                job_.visitId.c_str(), phaseName(job_.phase));

  const uint32_t start = millis();
  unsigned failures = 0;
  bool announced = false;
  for (;;) {
    if (failures >= cfg::kAttemptsPerSession || millis() - start > cfg::kSessionBudgetMs)
      return goPending();
    if (failures)
      sleepFed(backoffMs(failures - 1, cfg::kBackoffBaseMs, cfg::kBackoffCapMs, esp_random() % 1000));
    esp_task_wdt_reset();
    if (!waitNetwork(cfg::kNetworkWaitMs)) {
      Serial.println("[FLOW] network not ready");
      ++failures;
      continue;
    }

    Outcome outcome;
    if (job_.phase == Phase::Recorded || job_.phase == Phase::Uploading) {
      outcome = uploadStep(announced);
    } else if (job_.phase == Phase::Uploaded) {
      outcome = completeStep();
      if (outcome == Outcome::Success) {
        Serial.printf("[FLOW] visit %s delivered\n", job_.visitId.c_str());
        emit_("UPLOAD_SUCCESS", job_.visitId);
        if (!slot_.remove()) Serial.println("[FLOW] WAV delete failed after success");
        store_.clear();
        job_ = Job();
        return Delivery::Done;
      }
    } else {
      drop("unexpected phase during delivery", true);
      return Delivery::Dropped;
    }

    if (outcome == Outcome::Fatal) {
      drop("server rejected the recording (non-retryable)", !job_.pending);
      return Delivery::Dropped;
    }
    if (outcome != Outcome::Success) ++failures;
  }
}

void RecordingFlow::service(bool retryNow) {
  if (idle()) return;
  if (job_.pending) {
    time_t now = time(nullptr);
    if (!job_.pendingSince && now > kValidEpoch) {
      job_.pendingSince = uint32_t(now);  // failed before NTP; start the TTL now
      store_.save(job_);
    }
    if (job_.pendingSince && now > kValidEpoch && uint32_t(now) - job_.pendingSince > cfg::kPendingTtlS) {
      drop("pending upload expired", false);
      return;
    }
  }
  if (!resumeDelivery_ && !job_.pending) return;
  if (!networkReady_()) return;
  if (retryNow || int32_t(millis() - nextRetryAt_) >= 0) deliver();
}
