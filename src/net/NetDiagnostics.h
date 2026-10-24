#pragma once
// Field diagnostics that turn "it does not connect" into a specific cause.
//
// Wi-Fi requirements of the ESP32 radio:
//   * 2.4 GHz only (802.11 b/g/n, channels 1-13). A 5 GHz-only SSID is invisible.
//   * WPA2-PSK or WPA/WPA2 mixed (WPA3-only and WPA2-Enterprise are not supported
//     by this firmware; open networks work but captive-portal login pages do not).
//   * Outbound TCP 443 (Web API + Storage) and the MQTT port (8883) must be open.
#include <Arduino.h>

namespace netdiag {

// Human-readable cause for an ESP-IDF Wi-Fi disconnect reason code.
const char *disconnectHint(uint8_t reason);

// Scans (2.4 GHz is all the ESP32 can hear) and reports whether `ssid` is
// visible, on which channel, its signal and security, plus similar names.
// Briefly interrupts any connection attempt in progress.
void scanFor(const String &ssid);

// Fallback clock when NTP (UDP 123) is blocked: reads the Date header of a
// plain-HTTP response from `host`. TLS still verifies certificates; this only
// supplies the approximate time those checks need.
bool timeFromHttpDate(const char *host);

// DNS + TCP reachability of a host:port, printed as one log line.
bool probe(const char *label, const char *host, uint16_t port);

}  // namespace netdiag
