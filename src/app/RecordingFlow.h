#pragma once
// Visit recording state machine (runs only in the worker task).
//
//   ASSIGN_VISIT -> Assigned -> Recording -> Recorded -> Uploading -> Uploaded -> done
//                                  |            \___________ delivery session ___________/
//                                  |                 (bounded attempts + budget)
//                                  v                           |
//                        reboot/mic fault: drop,       exhausted: pending, retried in the
//                        UPLOAD_FAILED, idle           background with backoff until TTL
//
// Every phase change is checkpointed to NVS before the side effect, so a
// reboot (including a watchdog reset) resumes from the last safe point and
// never re-records a visit or uploads to a different recording ID.
#include <Arduino.h>
#include "audio/Recorder.h"
#include "core/RetryPolicy.h"
#include "net/RecordingApi.h"
#include "storage/Storage.h"

using EventSink = void (*)(const char *type, const String &visitId);
using ReadyCheck = bool (*)();

class RecordingFlow {
 public:
  RecordingFlow(EventSink emit, ReadyCheck networkReady)
      : emit_(emit), networkReady_(networkReady) {}

  bool begin();
  void resume();                // once after boot
  void assign(const String &messageId, const String &visitId);
  void service(bool retryNow);  // periodic: background delivery retries
  bool idle() const { return job_.phase == Phase::None; }

 private:
  enum class Delivery { Done, Pending, Dropped };

  JobStore store_;
  WavSlot slot_;
  Recorder recorder_;
  RecordingApi api_;
  Job job_;
  EventSink emit_;
  ReadyCheck networkReady_;
  bool resumeDelivery_ = false;
  uint32_t nextRetryAt_ = 0;

  bool checkpoint(Phase phase);
  bool record();
  Delivery deliver();
  Outcome uploadStep(bool &announced);
  Outcome completeStep();
  Delivery goPending();
  void drop(const char *reason, bool notify);
  bool waitNetwork(uint32_t timeoutMs);
  static void sleepFed(uint32_t ms);
  static uint32_t pcmBytes();
};
