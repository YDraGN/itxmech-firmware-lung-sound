#pragma once
// Consumer side for production: drains Capture into a WAV file on LittleFS.
#include <FS.h>
#include "audio/Capture.h"

class Recorder {
 public:
  bool begin() { return capture_.begin(); }
  // Records exactly `seconds` of audio into `file` (header + PCM).
  bool record(File &file, uint32_t seconds, CaptureStats &stats);

 private:
  Capture capture_;
};
