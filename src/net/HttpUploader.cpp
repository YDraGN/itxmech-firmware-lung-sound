#include "net/HttpUploader.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include "core/Config.h"

namespace {
constexpr size_t kChunk = 4096;
constexpr size_t kMaxErrorBody = 256;

bool splitUrl(const String &url, String &host, String &path) {
  if (!url.startsWith("https://")) return false;
  int slash = url.indexOf('/', 8);
  if (slash <= 8) return false;
  host = url.substring(8, slash);
  path = url.substring(slash);
  return host.indexOf(':') < 0 && host.indexOf('@') < 0;
}

// Reads one CRLF-terminated line before `deadline`; false on timeout/close.
bool readLine(WiFiClientSecure &tls, String &line, uint32_t deadline) {
  line = "";
  while (int32_t(millis() - deadline) < 0) {
    while (tls.available()) {
      char c = char(tls.read());
      if (c == '\n') return true;
      if (c != '\r' && line.length() < 512) line += c;
    }
    if (!tls.connected()) return false;
    delay(5);
  }
  return false;
}
}  // namespace

bool HttpUploader::heapOkForTls() {
  return ESP.getFreeHeap() >= cfg::kMinTlsFreeHeap &&
         ESP.getMaxAllocHeap() >= cfg::kMinTlsHeapBlock;
}

const char *HttpUploader::errorName(int code) {
  switch (code) {
    case kErrUrl: return "BAD_URL";
    case kErrHeap: return "LOW_HEAP";
    case kErrConnect: return "CONNECT_FAILED";
    case kErrWrite: return "WRITE_FAILED";
    case kErrStall: return "STALLED";
    case kErrDeadline: return "DEADLINE";
    case kErrNoResponse: return "NO_RESPONSE";
    case kErrBadResponse: return "BAD_RESPONSE";
    case kErrFile: return "FILE_READ";
    default: return code < 0 ? "TRANSPORT" : "HTTP";
  }
}

int HttpUploader::put(const String &url, const char *caCert, File &file,
                      const char *contentType, UploadReport &report) {
  report = UploadReport();
  report.freeHeap = ESP.getFreeHeap();
  report.maxBlock = ESP.getMaxAllocHeap();
  report.rssi = WiFi.RSSI();
  String host, path;
  if (!splitUrl(url, host, path)) return kErrUrl;
  if (!file || !file.seek(0)) return kErrFile;
  const uint32_t size = file.size();
  // A TLS session needs one large contiguous block; fragmentation shows here.
  if (!heapOkForTls()) return kErrHeap;

  WiFiClientSecure tls;
  tls.setCACert(caCert);
  tls.setHandshakeTimeout(cfg::kTlsHandshakeS);
  uint32_t t0 = millis();
  // The timeout also bounds every later TLS write (ssl_client socket_timeout).
  if (!tls.connect(host.c_str(), 443, cfg::kUploadStallMs)) {
    char reason[96] = "";
    tls.lastError(reason, sizeof(reason));
    report.error = reason;
    return kErrConnect;
  }
  report.connectMs = millis() - t0;
  esp_task_wdt_reset();

  String head = String("PUT ") + path + " HTTP/1.1\r\nHost: " + host +
                "\r\nContent-Type: " + contentType +
                "\r\nContent-Length: " + String(size) +
                "\r\nConnection: close\r\nUser-Agent: respicare-lung-sound\r\n\r\n";
  if (tls.write(reinterpret_cast<const uint8_t *>(head.c_str()), head.length()) != head.length()) {
    tls.stop();
    return kErrWrite;
  }

  // Total budget: fixed overhead + size at the minimum acceptable throughput.
  const uint32_t budget = 15000 + uint32_t(uint64_t(size) * 1000 / cfg::kUploadMinBytesPerS);
  const uint32_t bodyStart = millis();
  uint32_t lastProgress = bodyStart;
  unsigned nextQuarter = 1;
  static uint8_t buffer[kChunk];
  int result = 0;
  while (report.bytesSent < size) {
    size_t want = min<size_t>(kChunk, size - report.bytesSent);
    if (file.read(buffer, want) != want) { result = kErrFile; break; }
    size_t offset = 0;
    while (offset < want) {
      uint32_t w0 = millis();
      int n = tls.write(buffer + offset, want - offset);
      uint32_t took = millis() - w0;
      if (took > report.slowestWriteMs) report.slowestWriteMs = took;
      if (n > 0) {
        offset += n;
        lastProgress = millis();
        esp_task_wdt_reset();
      } else if (!tls.connected()) {
        result = kErrWrite;
        break;
      } else if (millis() - lastProgress > cfg::kUploadStallMs) {
        result = kErrStall;
        break;
      }
      if (millis() - bodyStart > budget) { result = kErrDeadline; break; }
    }
    if (result) break;
    report.bytesSent += want;
    if (report.bytesSent * 4 >= size * nextQuarter) {
      uint32_t ms = millis() - bodyStart;
      Serial.printf("[UPLOAD] %3u%%  %u/%u B  %.1f KB/s  slowest write %u ms  rssi %d\n",
                    nextQuarter * 25, unsigned(report.bytesSent), unsigned(size),
                    ms ? report.bytesSent / 1.024f / ms : 0.0f,
                    unsigned(report.slowestWriteMs), WiFi.RSSI());
      ++nextQuarter;
    }
  }
  report.bodyMs = millis() - bodyStart;
  if (result) {
    tls.stop();
    return result;
  }

  // Status line, then headers; the body is kept only for error diagnosis.
  uint32_t r0 = millis();
  const uint32_t deadline = r0 + cfg::kResponseTimeoutMs;
  String line;
  if (!readLine(tls, line, deadline)) { tls.stop(); return kErrNoResponse; }
  int code = 0;
  if (!line.startsWith("HTTP/1.") || line.length() < 12 ||
      (code = line.substring(9, 12).toInt()) < 100) {
    tls.stop();
    return kErrBadResponse;
  }
  while (readLine(tls, line, deadline) && line.length()) {}
  if (code >= 300) {
    while (int32_t(millis() - deadline) < 0 && report.error.length() < kMaxErrorBody &&
           (tls.available() || tls.connected())) {
      if (tls.available()) report.error += char(tls.read());
      else delay(5);
    }
  }
  report.responseMs = millis() - r0;
  tls.stop();
  return code;
}
