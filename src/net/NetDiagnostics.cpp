#include "net/NetDiagnostics.h"
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

namespace {
const char *authName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "OTHER";
  }
}

String lower(String value) {
  value.toLowerCase();
  value.replace(" ", "");
  value.replace("-", "");
  value.replace("_", "");
  return value;
}
}  // namespace

namespace netdiag {

const char *disconnectHint(uint8_t reason) {
  switch (reason) {
    case 201: return "SSID not heard on 2.4 GHz: 5 GHz-only network, hidden, out of range or name typo";
    case 202: return "authentication rejected: wrong password or unsupported security (WPA3-only/Enterprise)";
    case 15:
    case 204: return "WPA handshake timed out: almost always a wrong password";
    case 2: return "authentication expired: weak signal or AP overloaded";
    case 3:
    case 8: return "AP or station left: router restarted or kicked the device";
    case 4:
    case 5: return "association rejected: AP full (too many clients) or MAC filtering";
    case 200: return "beacon timeout: signal lost (distance, metal enclosure)";
    case 203: return "association failed: AP refused the device";
    default: return "see ESP-IDF wifi_err_reason_t";
  }
}

void scanFor(const String &ssid) {
  Serial.println("[WIFI-DIAG] scanning (ESP32 radio is 2.4 GHz only)...");
  // A scan cannot run while the driver is busy joining; pause the attempt.
  WiFi.disconnect(false, false);
  delay(100);
  int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    Serial.printf("[WIFI-DIAG] scan failed (%d)\n", count);
    return;
  }
  int best = -1, hidden = 0;
  String similar;
  for (int i = 0; i < count; ++i) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) ++hidden;
    if (name == ssid) {
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    } else if (!name.isEmpty() &&
               (lower(name).indexOf(lower(ssid)) >= 0 || lower(ssid).indexOf(lower(name)) >= 0)) {
      if (similar.length() < 200) similar += " \"" + name + "\"";
    }
  }
  Serial.printf("[WIFI-DIAG] %d network(s) on 2.4 GHz (%d hidden)\n", count, hidden);
  for (int i = 0; i < count && i < 8; ++i)
    Serial.printf("[WIFI-DIAG]   ch%-2d %4d dBm %-15s \"%s\"\n", WiFi.channel(i), WiFi.RSSI(i),
                  authName(WiFi.encryptionType(i)), WiFi.SSID(i).c_str());

  if (best < 0) {
    Serial.printf("[WIFI-DIAG] RESULT: \"%s\" is NOT visible on 2.4 GHz.\n", ssid.c_str());
    Serial.println("[WIFI-DIAG]   If a phone/laptop sees it, the network is 5 GHz-only (or a "
                   "band-steered combined SSID): enable a 2.4 GHz SSID on the router/hotspot.");
    Serial.println("[WIFI-DIAG]   Otherwise check the exact name (case, spaces) and distance.");
    if (similar.length()) Serial.printf("[WIFI-DIAG]   Similar 2.4 GHz names:%s\n", similar.c_str());
    if (hidden) Serial.println("[WIFI-DIAG]   Hidden SSIDs exist; a hidden target needs the exact name.");
  } else {
    wifi_auth_mode_t auth = WiFi.encryptionType(best);
    int rssi = WiFi.RSSI(best);
    Serial.printf("[WIFI-DIAG] RESULT: \"%s\" visible on 2.4 GHz ch%d, %d dBm, %s\n",
                  ssid.c_str(), WiFi.channel(best), rssi, authName(auth));
    if (auth == WIFI_AUTH_WPA2_ENTERPRISE)
      Serial.println("[WIFI-DIAG]   Enterprise (802.1X) login is not supported: use a WPA2-PSK network.");
    else if (auth == WIFI_AUTH_WPA3_PSK)
      Serial.println("[WIFI-DIAG]   WPA3-only: switch the router to WPA2/WPA3 mixed or WPA2-PSK.");
    else if (auth == WIFI_AUTH_WEP)
      Serial.println("[WIFI-DIAG]   WEP is obsolete and not supported: use WPA2-PSK.");
    else if (auth == WIFI_AUTH_OPEN)
      Serial.println("[WIFI-DIAG]   Open network: if it shows a login/accept page, the device cannot pass it.");
    else
      Serial.println("[WIFI-DIAG]   Band and security are fine; if joining still fails the password is wrong.");
    if (rssi < -75)
      Serial.println("[WIFI-DIAG]   Signal weak (< -75 dBm): move closer; uploads will be slow or stall.");
  }
  WiFi.scanDelete();
}

bool timeFromHttpDate(const char *host) {
  WiFiClient client;
  if (!client.connect(host, 80, 5000)) {
    Serial.printf("[TIME] HTTP fallback: cannot reach %s:80\n", host);
    return false;
  }
  client.printf("HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
  uint32_t deadline = millis() + 5000;
  String line;
  bool ok = false;
  while (int32_t(millis() - deadline) < 0 && (client.connected() || client.available())) {
    if (!client.available()) { delay(10); continue; }
    char c = char(client.read());
    if (c != '\n') {
      if (c != '\r' && line.length() < 128) line += c;
      continue;
    }
    if (line.isEmpty()) break;  // end of headers
    if (line.startsWith("Date:") || line.startsWith("date:")) {
      struct tm value = {};
      if (strptime(line.c_str() + 5, " %a, %d %b %Y %H:%M:%S GMT", &value)) {
        // configTime(0, 0, ...) keeps TZ at UTC, so mktime() is timegm().
        struct timeval now = {mktime(&value), 0};
        ok = now.tv_sec > 1735689600 && settimeofday(&now, nullptr) == 0;
      }
      break;
    }
    line = "";
  }
  client.stop();
  Serial.printf("[TIME] HTTP Date fallback from %s: %s\n", host, ok ? "clock set" : "failed");
  return ok;
}

bool probe(const char *label, const char *host, uint16_t port) {
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    Serial.printf("[NET-DIAG] %-7s %s: DNS lookup FAILED (DNS blocked or no internet)\n", label, host);
    return false;
  }
  WiFiClient client;
  uint32_t t0 = millis();
  bool ok = client.connect(ip, port, 5000);
  uint32_t ms = millis() - t0;
  client.stop();
  if (ok)
    Serial.printf("[NET-DIAG] %-7s %s:%u (%s) reachable in %u ms\n", label, host, port,
                  ip.toString().c_str(), unsigned(ms));
  else
    Serial.printf("[NET-DIAG] %-7s %s:%u (%s) TCP BLOCKED/unreachable: the network "
                  "firewall drops this port (common on cafe/guest Wi-Fi)\n",
                  label, host, port, ip.toString().c_str());
  return ok;
}

}  // namespace netdiag
