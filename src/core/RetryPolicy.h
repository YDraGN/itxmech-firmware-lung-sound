#pragma once
// HTTP result classification and backoff. Pure C++ for host tests.
#include <stdint.h>

enum class Outcome : uint8_t {
  Success,     // 2xx
  Retry,       // transport error, 408, 429, 5xx: same step again after backoff
  RefreshUrl,  // signed Storage URL rejected/expired: get a new URL, then retry
  Fatal,       // contract/config error: retrying cannot help
};

// Negative codes are local transport errors (see net/HttpUploader.h and the
// Arduino HTTPClient HTTPC_ERROR_* values).
inline Outcome classifyApi(int code) {
  if (code >= 200 && code < 300) return Outcome::Success;
  if (code <= 0 || code == 408 || code == 429 || code >= 500) return Outcome::Retry;
  return Outcome::Fatal;
}

inline Outcome classifyStoragePut(int code) {
  // Supabase signed upload tokens report expiry/invalid token as 400/401/403.
  if (code == 400 || code == 401 || code == 403) return Outcome::RefreshUrl;
  return classifyApi(code);
}

// Exponential backoff: base * 2^attempt, capped, plus up to `jitter` ms.
inline uint32_t backoffMs(unsigned attempt, uint32_t base, uint32_t cap, uint32_t jitter) {
  uint32_t delay = cap;
  if (attempt < 16 && (base << attempt) >> attempt == base) {
    uint32_t value = base << attempt;
    if (value < cap) delay = value;
  }
  return delay + jitter;
}
