#pragma once
// Streams a file to an HTTPS URL with a hand-written PUT so every stage has
// its own bound and is visible in the log:
//   heap check -> TCP+TLS connect -> headers -> 4 KiB body chunks -> status line
// Guards against the hangs seen with a single blocking sendRequest():
//   * per-write socket timeout (TLS layer) + stall detector (no progress)
//   * total deadline sized from the file length and a minimum throughput
//   * status-line wait bounded separately from the body
// The task watchdog is fed after every chunk that makes progress.
#include <Arduino.h>
#include <FS.h>

struct UploadReport {
  uint32_t bytesSent = 0;
  uint32_t connectMs = 0;
  uint32_t bodyMs = 0;
  uint32_t responseMs = 0;
  uint32_t slowestWriteMs = 0;
  uint32_t freeHeap = 0;
  uint32_t maxBlock = 0;
  int rssi = 0;
  String error;  // first bytes of an error response body, for diagnosis
};

class HttpUploader {
 public:
  // Local failures, all retryable (see core/RetryPolicy.h).
  static constexpr int kErrUrl = -10;
  static constexpr int kErrHeap = -11;
  static constexpr int kErrConnect = -12;
  static constexpr int kErrWrite = -13;
  static constexpr int kErrStall = -14;
  static constexpr int kErrDeadline = -15;
  static constexpr int kErrNoResponse = -16;
  static constexpr int kErrBadResponse = -17;
  static constexpr int kErrFile = -18;

  static const char *errorName(int code);
  // Enough free heap, and a large enough contiguous block, for one more TLS session.
  static bool heapOkForTls();

  // Returns the HTTP status code, or a negative kErr* value.
  int put(const String &url, const char *caCert, File &file,
          const char *contentType, UploadReport &report);
};
