#pragma once
// Compile-time constants shared by every layer. Secrets and per-device values
// live in the generated BuildConfig.h, never here.
#include <stddef.h>
#include <stdint.h>

namespace cfg {

// ---- Audio format (must match the server WAV validator) ----
constexpr uint32_t kSampleRate = 16000;
constexpr uint16_t kChannels = 1;
constexpr uint16_t kBitsPerSample = 16;
constexpr uint32_t kBytesPerSecond = kSampleRate * kChannels * kBitsPerSample / 8;
constexpr uint32_t kMaxWavBytes = 50UL * 1024 * 1024;

// ---- INMP441 on ESP32 38-pin devkit (pins from the hardware bring-up sketch) ----
constexpr int kPinI2sSck = 18;
constexpr int kPinI2sWs = 19;
constexpr int kPinI2sSd = 34;  // input-only pin

// ---- Capture pipeline ----
constexpr size_t kBlockFrames = 512;         // one DMA buffer = 32 ms
constexpr int kDmaBufCount = 16;             // 512 ms of slack inside the driver
constexpr size_t kRingBytes = 32 * 1024;     // ~1 s between capture and flash writer
constexpr uint32_t kWarmupMs = 500;          // mic restarts per recording (~256 ms) + filter settling
constexpr size_t kFlashChunkSamples = 2048;  // 4 KiB writes to LittleFS

// ---- Network timeouts (ms) ----
constexpr uint32_t kTlsHandshakeS = 15;
constexpr uint32_t kHttpConnectMs = 15000;
constexpr uint32_t kApiTimeoutMs = 20000;       // upload-url
constexpr uint32_t kCompleteTimeoutMs = 60000;  // complete may verify WAV server-side
constexpr uint32_t kUploadStallMs = 20000;      // no byte accepted for this long -> abort
constexpr uint32_t kUploadMinBytesPerS = 4096;  // floor used to size the total deadline
constexpr uint32_t kResponseTimeoutMs = 30000;  // wait for status line after the body
// A TLS session allocates ~40 KB in total; its largest piece is the 16 KB
// record buffer. Check both so fragmentation is reported, not crashed on.
constexpr uint32_t kMinTlsFreeHeap = 40000;
constexpr uint32_t kMinTlsHeapBlock = 20000;

// ---- Retry policy ----
constexpr unsigned kAttemptsPerSession = 4;
constexpr uint32_t kBackoffBaseMs = 2000;
constexpr uint32_t kBackoffCapMs = 30000;
constexpr uint32_t kSessionBudgetMs = 5UL * 60 * 1000;   // one delivery session
constexpr uint32_t kPendingRetryMinMs = 60UL * 1000;     // background retries
constexpr uint32_t kPendingRetryMaxMs = 10UL * 60 * 1000;
constexpr uint32_t kPendingTtlS = 24UL * 60 * 60;        // then the WAV is dropped
constexpr uint32_t kNetworkWaitMs = 20000;

// ---- Watchdog ----
// Longer than the slowest single blocking call (TLS handshake + complete read).
constexpr uint32_t kTaskWatchdogS = 120;

}  // namespace cfg
