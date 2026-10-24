#pragma once
// Persistent state: the single WAV slot on LittleFS and the job checkpoint in NVS.
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

enum class Phase : uint8_t {
  None,       // idle, nothing stored
  Assigned,   // command accepted, nothing recorded yet
  Recording,  // capture in progress (a reboot here means the audio is lost)
  Recorded,   // WAV complete locally, upload not started
  Uploading,  // backend recording ID known, PUT may be partial
  Uploaded,   // Storage accepted the PUT, complete not acknowledged
};

const char *phaseName(Phase phase);

struct Job {
  String messageId;
  String visitId;
  String recordingId;
  Phase phase = Phase::None;
  bool pending = false;        // delivery session exhausted; background retries
  uint32_t pendingSince = 0;   // UTC seconds of the first failed session
  uint16_t sessions = 0;       // delivery sessions run so far
};

class JobStore {
 public:
  bool begin();
  bool load(Job &job);
  bool save(const Job &job);
  bool clear();
  // MQTT QoS1 may redeliver; remember recent message IDs (bounded ring).
  bool seenMessage(const String &messageId);
  void rememberMessage(const String &messageId);

 private:
  Preferences prefs_;
};

class WavSlot {
 public:
  // Mounts LittleFS. Formats only when mounting fails AND no job references
  // a stored WAV (fresh or replaced MCU), so recorded audio is never erased.
  bool begin(bool jobStored);
  File create() { return LittleFS.open(kPath, "w+"); }
  File open() { return LittleFS.open(kPath, "r"); }
  bool exists();
  bool remove() { return !exists() || LittleFS.remove(kPath); }
  bool hasRoom(uint32_t bytes);
  bool valid(uint32_t pcmBytes);

 private:
  static constexpr const char *kPath = "/recording.wav";
};
