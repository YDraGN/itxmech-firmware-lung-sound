#include "storage/Storage.h"
#include <ArduinoJson.h>
#include <sys/stat.h>
#include "audio/Wav.h"

namespace {
constexpr const char *kNamespace = "lungsound";
constexpr size_t kRecentMessages = 16;

const char *const kPhaseNames[] = {"none", "assigned", "recording",
                                   "recorded", "uploading", "uploaded"};

Phase phaseFrom(const String &name) {
  for (size_t i = 0; i < sizeof(kPhaseNames) / sizeof(*kPhaseNames); ++i)
    if (name == kPhaseNames[i]) return Phase(i);
  return Phase::None;
}
}  // namespace

const char *phaseName(Phase phase) { return kPhaseNames[size_t(phase)]; }

// ---------------- JobStore ----------------

bool JobStore::begin() { return prefs_.begin(kNamespace, false); }

bool JobStore::load(Job &job) {
  if (!prefs_.isKey("job")) return false;
  String value = prefs_.getString("job");
  if (value.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, value)) {
    Serial.println("[STORE] unreadable checkpoint; treating as lost recording");
    job = Job();
    job.phase = Phase::Recording;  // forces the "audio lost" path on resume
    return true;
  }
  job.messageId = doc["message"].as<String>();
  job.visitId = doc["visit"].as<String>();
  job.recordingId = doc["recording"] | "";
  job.phase = phaseFrom(doc["phase"].as<String>());
  job.pending = doc["pending"] | false;
  job.pendingSince = doc["since"] | 0U;
  job.sessions = doc["sessions"] | 0U;
  return job.phase != Phase::None;
}

bool JobStore::save(const Job &job) {
  JsonDocument doc;
  doc["message"] = job.messageId;
  doc["visit"] = job.visitId;
  doc["recording"] = job.recordingId;
  doc["phase"] = phaseName(job.phase);
  doc["pending"] = job.pending;
  doc["since"] = job.pendingSince;
  doc["sessions"] = job.sessions;
  String value;
  serializeJson(doc, value);
  return prefs_.putString("job", value) == value.length();
}

bool JobStore::clear() { return !prefs_.isKey("job") || prefs_.remove("job"); }

bool JobStore::seenMessage(const String &messageId) {
  String recent = prefs_.isKey("recent") ? prefs_.getString("recent") : String();
  return recent.indexOf(messageId) >= 0;
}

void JobStore::rememberMessage(const String &messageId) {
  // Space-separated, newest last, at most kRecentMessages UUIDs.
  String recent = prefs_.isKey("recent") ? prefs_.getString("recent") : String();
  if (!recent.isEmpty()) recent += ' ';
  recent += messageId;
  const size_t maxLen = kRecentMessages * 37 - 1;
  if (recent.length() > maxLen) recent = recent.substring(recent.length() - maxLen);
  prefs_.putString("recent", recent);
}

// ---------------- WavSlot ----------------

bool WavSlot::begin(bool jobStored) {
  if (LittleFS.begin(false)) return true;
  if (jobStored) {
    Serial.println("[STORE] LittleFS mount failed with a job stored; not formatting");
    return false;
  }
  Serial.println("[STORE] LittleFS unformatted (new MCU?); formatting empty partition");
  return LittleFS.begin(true);
}

bool WavSlot::exists() {
  // stat() instead of LittleFS.exists(): the latter opens the file and logs
  // a spurious "[E] ... does not exist" line when it is absent.
  struct stat info;
  return stat((String("/littlefs") + kPath).c_str(), &info) == 0;
}

bool WavSlot::hasRoom(uint32_t bytes) {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  return used <= total && total - used >= bytes + 32768;
}

bool WavSlot::valid(uint32_t pcmBytes) {
  File file = open();
  if (!file || file.size() != pcmBytes + wav::kHeaderBytes) return false;
  uint8_t header[wav::kHeaderBytes];
  return file.read(header, sizeof(header)) == sizeof(header) &&
         wav::validHeader(header, pcmBytes);
}
