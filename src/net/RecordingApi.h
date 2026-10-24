#pragma once
// Device endpoints of itxmech-respiratory-web (X-Device-Key auth):
//   POST /api/device/visits/{visitId}/recording/upload-url -> signed Storage URL
//   PUT  <signed Supabase URL>                              -> WAV bytes (no device key)
//   POST /api/device/recordings/{recordingId}/complete      -> server verifies WAV
#include <Arduino.h>
#include <FS.h>
#include "net/HttpUploader.h"
#include "storage/Storage.h"

struct UploadTicket {
  String url;
  uint32_t issuedAt = 0;
  uint32_t lifetimeMs = 0;
  bool fresh() const {
    return lifetimeMs > 30000 && uint32_t(millis() - issuedAt) < lifetimeMs - 30000;
  }
};

class RecordingApi {
 public:
  // 409 RECORDING_ALREADY_UPLOADED: Storage already has the file; go to complete.
  static constexpr int kAlreadyUploaded = 1409;

  int requestUploadUrl(Job &job, UploadTicket &ticket);
  int upload(const UploadTicket &ticket, File &file, UploadReport &report);
  int complete(const Job &job, uint32_t fileBytes);

 private:
  HttpUploader uploader_;
};
