// Bench streaming apps for the hardware team. Same capture + DSP path as
// production, but PCM goes to a PC instead of a WAV file:
//   stream_serial: raw int16 LE PCM on USB serial (tools/serial_record.py).
//                  Nothing else is printed after start-up, so the stream stays clean.
//   stream_udp:    packets [ "LSP1" | seq u32 LE | 512 x int16 LE ] to UDP_HOST:UDP_PORT
//                  (tools/udp_record.py fills missing sequence numbers by repetition).
#include <Arduino.h>
#include <BuildConfig.h>
#include "audio/Capture.h"
#include "core/AppEntry.h"
#include "core/Config.h"

#if APP_MODE_STREAM_UDP
#include <WiFi.h>
#include <WiFiUdp.h>
#endif

namespace {
Capture capture;
bool ready = false;
int16_t pcm[cfg::kBlockFrames];

#if APP_MODE_STREAM_UDP
WiFiUDP udp;
IPAddress target;
uint32_t sequence = 0;
uint8_t packet[8 + sizeof(pcm)];

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds bursty latency and packet loss
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] connecting");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("[WIFI] %s rssi %d -> %s:%d\n", WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), UDP_HOST, UDP_PORT);
  return target.fromString(UDP_HOST);
}
#endif
}  // namespace

void appSetup() {
  Serial.begin(921600);
  delay(300);
#if APP_MODE_STREAM_UDP
  if (!connectWifi()) {
    Serial.println("[WIFI] connection failed; restarting in 5 s");
    delay(5000);
    ESP.restart();
  }
#endif
  ready = capture.begin() && capture.start();
  Serial.println(ready ? "[STREAM] started" : "[STREAM] capture init failed");
  Serial.flush();
}

void appLoop() {
  if (!ready) {
    delay(1000);
    return;
  }
  // Always ship full blocks so a lost UDP packet is exactly kBlockFrames long.
  static size_t have = 0;
  have += capture.read(pcm + have, cfg::kBlockFrames - have, 200);
  if (have < cfg::kBlockFrames) return;
  const size_t n = have;
  have = 0;
#if APP_MODE_STREAM_SERIAL
  Serial.write(reinterpret_cast<uint8_t *>(pcm), n * sizeof(int16_t));
#else
  if (WiFi.status() != WL_CONNECTED) return;  // auto-reconnect; PC conceals the gap
  memcpy(packet, "LSP1", 4);
  for (int i = 0; i < 4; ++i) packet[4 + i] = uint8_t(sequence >> (8 * i));
  memcpy(packet + 8, pcm, n * sizeof(int16_t));
  udp.beginPacket(target, UDP_PORT);
  udp.write(packet, 8 + n * sizeof(int16_t));
  udp.endPacket();
  ++sequence;
#endif
}
