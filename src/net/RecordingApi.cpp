#include "net/RecordingApi.h"
#include <ArduinoJson.h>
#include <BuildConfig.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "audio/Wav.h"
#include "core/Config.h"
#include "net/certs/StorageCa.h"
#include "net/certs/WebApiCa.h"
#include "proto/Protocol.h"

namespace {
// HTTPClient decodes chunking into this bounded sink; never buffer a WAV in RAM.
class ResponseBuffer : public Stream {
 public:
  String value;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (value.length() + size > 8192) return 0;
    return value.concat(reinterpret_cast<const char *>(buffer), size) ? size : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void configure(HTTPClient &http, WiFiClientSecure &tls, uint32_t timeoutMs) {
  // Vercel's chain ends in a cross-signed GTS root; pin the self-signed root.
  tls.setCACert(kWebApiCa);
  tls.setHandshakeTimeout(cfg::kTlsHandshakeS);
  http.setConnectTimeout(cfg::kHttpConnectMs);
  http.setTimeout(timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
}

bool allowedUploadUrl(const String &url) {
  return url.startsWith(String("https://") + UPLOAD_HOST + "/") &&
         url.indexOf('#') < 0 && url.length() <= 4096;
}

int post(const String &endpoint, const String &body, uint32_t timeoutMs, String *response) {
  if (!HttpUploader::heapOkForTls()) {
    Serial.printf("[API] low heap for TLS: free %u, block %u\n", unsigned(ESP.getFreeHeap()),
                  unsigned(ESP.getMaxAllocHeap()));
    return HttpUploader::kErrHeap;
  }
  WiFiClientSecure tls;
  HTTPClient http;
  configure(http, tls, timeoutMs);
  if (!http.begin(tls, endpoint)) return HttpUploader::kErrUrl;
  http.addHeader("X-Device-Key", FIRMWARE_DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code > 0 && response) {
    ResponseBuffer sink;
    if (http.writeToStream(&sink) < 0) code = HttpUploader::kErrBadResponse;
    else *response = sink.value;
  }
  http.end();
  return code;
}
}  // namespace

int RecordingApi::requestUploadUrl(Job &job, UploadTicket &ticket) {
  String body;
  int code = post(String(WEB_BASE_URL) + "/api/device/visits/" + job.visitId +
                      "/recording/upload-url",
                  "{}", cfg::kApiTimeoutMs, &body);
  if (code == 409 && !job.recordingId.isEmpty() &&
      body.indexOf("RECORDING_ALREADY_UPLOADED") >= 0)
    return kAlreadyUploaded;
  if (code < 200 || code >= 300) {
    if (code > 0) Serial.printf("[API] upload-url %d: %.200s\n", code, body.c_str());
    return code;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return HttpUploader::kErrBadResponse;
  JsonVariant data = doc["data"];
  String id = data["recording_id"].as<String>();
  String visit = data["visit_id"].as<String>();
  String url = data["upload_url"].as<String>();
  uint32_t expiry = data["expires_in"] | 0U;
  if (!validUuid(id) || visit != job.visitId || !allowedUploadUrl(url) ||
      expiry <= 30 || expiry > 86400 ||
      (!job.recordingId.isEmpty() && job.recordingId != id)) {
    Serial.println("[API] upload-url response violates the contract; not uploading");
    return 422;  // Fatal: never send audio to an unexpected place.
  }
  job.recordingId = id;
  ticket.url = url;
  ticket.issuedAt = millis();
  ticket.lifetimeMs = expiry * 1000;
  return code;
}

int RecordingApi::upload(const UploadTicket &ticket, File &file, UploadReport &report) {
  if (!ticket.fresh()) return 403;  // treated as "refresh the URL"
  if (!allowedUploadUrl(ticket.url)) return HttpUploader::kErrUrl;
  // Deliberately no device key on the Storage host.
  return uploader_.put(ticket.url, kStorageCa, file, "audio/wav", report);
}

int RecordingApi::complete(const Job &job, uint32_t fileBytes) {
  JsonDocument meta;
  uint32_t pcm = fileBytes > wav::kHeaderBytes ? fileBytes - wav::kHeaderBytes : 0;
  meta["file_size_bytes"] = fileBytes;
  meta["duration_ms"] = uint32_t(uint64_t(pcm) * 1000 / cfg::kBytesPerSecond);
  meta["sample_rate_hz"] = cfg::kSampleRate;
  meta["bit_depth"] = cfg::kBitsPerSample;
  meta["channel_count"] = cfg::kChannels;
  String body;
  serializeJson(meta, body);
  String response;
  int code = post(String(WEB_BASE_URL) + "/api/device/recordings/" + job.recordingId +
                      "/complete",
                  body, cfg::kCompleteTimeoutMs, &response);
  if (code > 0 && (code < 200 || code >= 300))
    Serial.printf("[API] complete %d: %.200s\n", code, response.c_str());
  return code;
}
