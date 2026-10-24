#include "net/WifiManager.h"
#include "net/NetDiagnostics.h"
#include "proto/Protocol.h"

#include <BuildConfig.h>
#include <WiFi.h>
#include <time.h>
#include <atomic>
#include <esp_sntp.h>
#include <ctype.h>

namespace {

std::atomic<bool> ntpSynced{false};
std::atomic<uint8_t> lastReason{0};
std::atomic<uint16_t> joinFailures{0};
uint32_t lastScanAt = 0;
uint32_t connectedAt = 0;
uint32_t lastHttpTimeAt = 0;
bool probed = false;

String webHost() {
  String host = String(WEB_BASE_URL).substring(8);  // after "https://"
  int slash = host.indexOf('/');
  return slash < 0 ? host : host.substring(0, slash);
}

void onNtpSync(struct timeval *) {
  ntpSynced.store(true);
}

// Wi-Fi disconnect reason codes.
// These values come from ESP-IDF Wi-Fi reason definitions.
const char* wifiReasonText(uint8_t reason) {
  switch (reason) {
    case 2:
      return "AUTH_EXPIRE";
    case 3:
      return "AUTH_LEAVE";
    case 4:
      return "ASSOC_EXPIRE";
    case 5:
      return "ASSOC_TOOMANY";
    case 15:
      return "4WAY_HANDSHAKE_TIMEOUT";
    case 201:
      return "NO_AP_FOUND";
    case 202:
      return "AUTH_FAIL";
    case 203:
      return "ASSOC_FAIL";
    case 204:
      return "HANDSHAKE_TIMEOUT";
    default:
      return "OTHER";
  }
}

}  // namespace

// ============================================================
// Wi-Fi Manager
// ============================================================

void WiFiManager::begin() {
  // Register events before starting Wi-Fi.
  // Do not print Wi-Fi passwords.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        Serial.println("[WIFI DEBUG] STA started");
        break;

      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println(
            "[WIFI DEBUG] Associated with access point");
        break;

      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf(
            "[WIFI DEBUG] Got IP: %s\n",
            WiFi.localIP().toString().c_str());
        break;

      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        uint8_t reason =
            info.wifi_sta_disconnected.reason;

        Serial.printf(
            "[WIFI DEBUG] Disconnected, reason=%u (%s): %s\n",
            unsigned(reason),
            wifiReasonText(reason),
            netdiag::disconnectHint(reason));
        lastReason.store(reason);
        joinFailures.fetch_add(1);
        break;
      }

      default:
        break;
    }
  });

  if (!settings.begin("wifi_setup", false)) {
    Serial.println("[SETUP] Failed to open NVS namespace");
  }

  // Preserve existing build-time credentials.
  // isKey() first: getString() on a missing key logs a spurious [E] line.
  ssid = settings.isKey("ssid") ? settings.getString("ssid") : String(WIFI_SSID);
  password = settings.isKey("password") ? settings.getString("password") : String(WIFI_PASSWORD);

  // Prefer saved credentials if available.
  JsonDocument credentials;
  String saved = settings.isKey("credentials") ? settings.getString("credentials") : String();

  if (saved.length() &&
      !deserializeJson(credentials, saved) &&
      credentials["ssid"].is<String>() &&
      credentials["password"].is<String>()) {

    ssid = credentials["ssid"].as<String>();
    password = credentials["password"].as<String>();

    Serial.println("[WIFI] Loaded saved credentials");

  } else if (ssid.length()) {
    Serial.println("[WIFI] Using configured credentials");
  } else {
    Serial.println("[WIFI] No saved credentials");
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (ssid.length()) {
    // Confirm up front that the network exists on 2.4 GHz and how it is secured.
    netdiag::scanFor(ssid);
    lastScanAt = millis();
    Serial.println("[WIFI] Connecting to configured network");

    WiFi.begin(
        ssid.c_str(),
        password.c_str());
  }

  lastAttempt = millis();
  disconnectedSince = lastAttempt;

  ntpSynced.store(false);
  wasConnected = false;
  wasSynced = false;

  configTime(
      0,
      0,
      "pool.ntp.org",
      "time.cloudflare.com");

  sntp_set_time_sync_notification_cb(onNtpSync);

  Serial.println(
      "[WIFI] Waiting for connection and UTC time");

  if (!ssid.length()) {
    startPortal();
  }
}

// ============================================================
// HTTP Helpers
// ============================================================

void WiFiManager::redirectToPortal() {
  portal.sendHeader("Location", "/", true);
  portal.sendHeader("Cache-Control", "no-store");
  portal.send(303, "text/plain", "");
}

// ============================================================
// Wi-Fi Setup Page
// ============================================================

void WiFiManager::showPortal() {
  // Accept requests only from the setup AP interface.
  if (portal.client().localIP() != WiFi.softAPIP()) {
    portal.send(
        403,
        "text/plain",
        "Connect to RespiCare Setup Wi-Fi first");
    return;
  }

  const bool busy = testing || connectPending;
  const bool success = closePortalAt != 0;

  String html = F(
      "<!doctype html>"
      "<html lang='vi'>"
      "<head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' "
      "content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='Cache-Control' "
      "content='no-store'>"
      "<title>RespiCare Wi-Fi Setup</title>"

      "<style>"
      "*{box-sizing:border-box}"
      "body{font:16px Arial,sans-serif;"
      "background:#f3f6fb;color:#172033;"
      "margin:0;padding:32px 16px}"
      ".card{max-width:460px;margin:auto;background:#fff;"
      "border-radius:14px;padding:26px;"
      "box-shadow:0 6px 24px rgba(0,0,0,.08)}"
      "h1{font-size:24px;margin:0 0 10px}"
      "p{line-height:1.6;color:#526072}"
      "label{display:block;font-weight:600;margin-top:18px}"
      "input{width:100%;padding:12px;margin-top:8px;"
      "border:1px solid #cbd5e1;border-radius:8px;"
      "font-size:16px}"
      "button{width:100%;padding:13px;margin-top:22px;"
      "background:#2563eb;color:#fff;border:0;"
      "border-radius:8px;font-size:16px;cursor:pointer}"
      "button:disabled{opacity:.6}"
      ".status{padding:14px;margin:20px 0;"
      "border-radius:8px;background:#eff6ff;"
      "color:#1e40af;line-height:1.6}"
      ".success{background:#ecfdf5;color:#065f46}"
      ".error{background:#fef2f2;color:#991b1b}"
      ".spin{width:24px;height:24px;"
      "border:3px solid #bfdbfe;"
      "border-top-color:#2563eb;border-radius:50%;"
      "animation:spin 1s linear infinite;"
      "margin:16px auto}"
      "@keyframes spin{to{transform:rotate(360deg)}}"
      ".hint{font-size:13px;color:#64748b}"
      "</style>"
      "</head>"

      "<body><div class='card'>"
      "<h1>RespiCare Wi-Fi Setup</h1>"
      "<p>Ket noi thiet bi voi mang Wi-Fi 2.4 GHz.</p>");

  // All portal messages are generated by firmware.
  // No user-supplied SSID/password is echoed here.
  html += "<div class='status";

  if (success) {
    html += " success";
  } else if (!busy &&
             (portalMessage.indexOf("thất bại") >= 0 ||
              portalMessage.indexOf("lưu thất bại") >= 0)) {
    html += " error";
  }

  html += "'>";
  html += portalMessage;
  html += "</div>";

  if (busy) {
    html += F(
        "<div class='spin'></div>"
        "<p style='text-align:center'>"
        "Dang thu ket noi Wi-Fi...</p>"
        "<p class='hint'>"
        "Thoi gian ket noi toi da 30 giay."
        "</p>"

        "<script>"
        "setTimeout(function(){"
        "window.location.replace('/');"
        "},3000);"
        "</script>");

  } else if (success) {
    html += F(
        "<h3>Da ket noi thanh cong!</h3>"
        "<p>Thong tin Wi-Fi da duoc luu.</p>"
        "<p>Thiet bi se dong Wi-Fi Setup sau vai giay.</p>"
        "<p class='hint'>"
        "Ket noi lai Internet va mo web RespiCare."
        "</p>");

  } else {
    html += F(
        "<form id='wifi-form' "
        "method='POST' action='/configure'>"

        "<label for='ssid'>Ten Wi-Fi (SSID)</label>"
        "<input id='ssid' name='ssid' "
        "maxlength='32' required autocomplete='off'>"

        "<label for='password'>Mat khau Wi-Fi</label>"
        "<input id='password' name='password' "
        "type='password' maxlength='64' "
        "autocomplete='off'>"

        "<button id='submit-btn' type='submit'>"
        "Ket noi"
        "</button>"
        "</form>"

        "<p class='hint'>"
        "Nhap Wi-Fi nha ban, "
        "khong phai mang RespiCare-Setup."
        "</p>"

        "<script>"
        "document.getElementById('wifi-form')"
        ".addEventListener('submit',function(){"
        "document.getElementById('submit-btn').disabled=true;"
        "});"
        "</script>");
  }

  html += F("</div></body></html>");

  portal.sendHeader("Cache-Control", "no-store");

  portal.send(
      200,
      "text/html; charset=utf-8",
      html);
}

// ============================================================
// Start Wi-Fi Setup Portal
// ============================================================

void WiFiManager::startPortal() {
  if (portalActive) {
    return;
  }

  String apPassword =
      settings.isKey("ap_password") ? settings.getString("ap_password") : String();

  if (apPassword.length() < 12) {
    char generated[17];

    snprintf(
        generated,
        sizeof(generated),
        "%08lx%08lx",
        (unsigned long)esp_random(),
        (unsigned long)esp_random());

    apPassword = generated;

    settings.putString(
        "ap_password",
        apPassword);
  }

  String apName =
      String("RespiCare-Setup-") +
      String(DEVICE_ID).substring(0, 16);

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPConfig(
      IPAddress(192, 168, 4, 1),
      IPAddress(192, 168, 4, 1),
      IPAddress(255, 255, 255, 0));

  if (!WiFi.softAP(
          apName.c_str(),
          apPassword.c_str())) {
    Serial.println("[SETUP] AP start failed");
    return;
  }

  testing = false;
  connectPending = false;
  closePortalAt = 0;

  portalMessage = "Nhập thông tin Wi-Fi mới.";

  // ----------------------------------------------------------
  // Main page
  // ----------------------------------------------------------

  portal.on("/", HTTP_GET, [this]() {
    showPortal();
  });

  // ----------------------------------------------------------
  // Configure GET
  // ----------------------------------------------------------

  portal.on("/configure", HTTP_GET, [this]() {
    redirectToPortal();
  });

  // ----------------------------------------------------------
  // Configure POST
  // ----------------------------------------------------------

  portal.on("/configure", HTTP_POST, [this]() {
    if (portal.client().localIP() != WiFi.softAPIP()) {
      portal.send(
          403,
          "text/plain",
          "Connect to device Setup Wi-Fi first");
      return;
    }

    // Do not start multiple connections simultaneously.
    if (testing || connectPending || closePortalAt) {
      Serial.println(
          "[SETUP] Duplicate connection request ignored");

      redirectToPortal();
      return;
    }

    String newSsid = portal.arg("ssid");
    String newPassword = portal.arg("password");

    bool validPassword =
        newPassword.isEmpty() ||
        (newPassword.length() >= 8 &&
         newPassword.length() <= 63);

    if (newPassword.length() == 64) {
      validPassword = true;

      for (unsigned i = 0; i < 64; ++i) {
        if (!isxdigit(
                (unsigned char)newPassword[i])) {
          validPassword = false;
          break;
        }
      }
    }

    if (newSsid.isEmpty() ||
        newSsid.length() > 32 ||
        !validPassword) {
      portal.send(
          400,
          "text/plain",
          "Invalid SSID or WPA password");
      return;
    }

    pendingSsid = newSsid;
    pendingPassword = newPassword;

    connectPending = true;

    portalMessage =
        "Đang thử kết nối. Chưa ghi đè cấu hình cũ.";

    Serial.printf(
        "[SETUP] Connection request accepted "
        "(SSID length=%u)\n",
        unsigned(newSsid.length()));

    redirectToPortal();
  });

  // ----------------------------------------------------------
  // Captive Portal Detection
  // ----------------------------------------------------------

  portal.on("/generate_204", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/gen_204", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/hotspot-detect.html", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/library/test/success.html", HTTP_GET,
      [this]() {
        redirectToPortal();
      });

  portal.on("/connecttest.txt", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/ncsi.txt", HTTP_GET, [this]() {
    redirectToPortal();
  });

  portal.on("/fwlink", HTTP_GET, [this]() {
    redirectToPortal();
  });

  // ----------------------------------------------------------
  // Not Found Handler
  // ----------------------------------------------------------

  portal.onNotFound([this]() {
    Serial.printf(
        "[HTTP] Unmatched request: %s, method=%d\n",
        portal.uri().c_str(),
        int(portal.method()));

    portal.sendHeader(
        "Location",
        "http://192.168.4.1/",
        true);

    portal.sendHeader(
        "Cache-Control",
        "no-store");

    portal.send(302, "text/plain", "");
  });

  dns.start(
      53,
      "*",
      WiFi.softAPIP());

  portal.begin();
  portalActive = true;

  Serial.printf(
      "[SETUP] SSID: %s\n",
      apName.c_str());

  Serial.printf(
      "[SETUP] Setup password: %s\n",
      apPassword.c_str());

  Serial.println(
      "[SETUP] URL: http://192.168.4.1/");
}

// ============================================================
// Stop Wi-Fi Setup Portal
// ============================================================

void WiFiManager::stopPortal() {
  portal.stop();
  dns.stop();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  portalActive = false;
  closePortalAt = 0;
  testing = false;
  connectPending = false;

  Serial.println("[SETUP] Portal closed");
}

// ============================================================
// Wi-Fi State Machine
// ============================================================

bool WiFiManager::tick() {
  uint32_t now = millis();

  // ----------------------------------------------------------
  // Handle HTTP/DNS
  // ----------------------------------------------------------

  if (portalActive) {
    dns.processNextRequest();
    portal.handleClient();
  }

  // ----------------------------------------------------------
  // New Wi-Fi connection request
  // ----------------------------------------------------------

  if (connectPending) {
    connectPending = false;
    testing = true;
    testStarted = now;

    Serial.println(
        "[SETUP] Starting Wi-Fi connection test");

    Serial.printf(
        "[SETUP] Target SSID length=%u\n",
        unsigned(pendingSsid.length()));

    // Disable automatic reconnect while testing.
    // This avoids competing reconnect attempts.
    WiFi.setAutoReconnect(false);

    // Disconnect STA without disabling the AP.
    WiFi.disconnect(false, false);

    // Ensure AP + STA is enabled.
    WiFi.mode(WIFI_AP_STA);

    wl_status_t startStatus =
        WiFi.begin(
            pendingSsid.c_str(),
            pendingPassword.c_str());

    Serial.printf(
        "[SETUP] WiFi.begin status=%d\n",
        int(startStatus));

    Serial.println(
        "[SETUP] Waiting up to 30 seconds");
  }

  bool connected =
      WiFi.status() == WL_CONNECTED;

  // ----------------------------------------------------------
  // Connection test succeeded
  // ----------------------------------------------------------

  if (testing &&
      connected &&
      WiFi.SSID() == pendingSsid) {

    Serial.println(
        "[SETUP] Wi-Fi connection successful");

    Serial.printf(
        "[SETUP] Assigned IP: %s\n",
        WiFi.localIP().toString().c_str());

    // Save credentials in a single NVS entry.
    JsonDocument credentials;

    credentials["ssid"] = pendingSsid;
    credentials["password"] = pendingPassword;

    String encoded;
    serializeJson(credentials, encoded);

    size_t written =
        settings.putString("credentials", encoded);

    if (written > 0) {
      ssid = pendingSsid;
      password = pendingPassword;

      portalMessage =
          "Kết nối thành công. Đã lưu cấu hình.";

      closePortalAt = now + 10000;

      Serial.println(
          "[SETUP] Credentials saved successfully");

      Serial.println(
          "[SETUP] Portal will close in 10 seconds");

    } else {
      portalMessage =
          "Kết nối được nhưng lưu thất bại. "
          "Vui lòng thử lại.";

      Serial.println(
          "[SETUP] ERROR: NVS save failed");

      // Restore old configuration.
      WiFi.disconnect(false, false);

      if (ssid.length()) {
        WiFi.begin(
            ssid.c_str(),
            password.c_str());
      }

      connected = false;
      lastAttempt = now;
      disconnectedSince = now;
    }

    pendingPassword = "";
    pendingSsid = "";
    testing = false;

    WiFi.setAutoReconnect(true);

  // ----------------------------------------------------------
  // Connection test timeout
  // ----------------------------------------------------------

  } else if (
      testing &&
      now - testStarted >= 30000) {

    Serial.printf(
        "[SETUP] Wi-Fi connection timeout, status=%d\n",
        int(WiFi.status()));

    Serial.println(
        "[SETUP] Check Wi-Fi 2.4 GHz, password, "
        "signal and router security");

    testing = false;

    pendingPassword = "";
    pendingSsid = "";

    portalMessage =
        "Kết nối thất bại. "
        "Cấu hình cũ vẫn được giữ. "
        "Vui lòng thử lại.";

    // Stop the failed connection attempt.
    WiFi.disconnect(false, false);

    // Restore previous credentials, if any.
    if (ssid.length()) {
      Serial.println(
          "[SETUP] Restoring previous Wi-Fi");

      WiFi.begin(
          ssid.c_str(),
          password.c_str());
    } else {
      Serial.println(
          "[SETUP] No previous Wi-Fi to restore");
    }

    WiFi.setAutoReconnect(true);

    lastAttempt = now;
    disconnectedSince = now;
    connected = false;
  }

  // ----------------------------------------------------------
  // Track network connection
  // ----------------------------------------------------------

  if (connected) {
    disconnectedSince = now;
  }

  // ----------------------------------------------------------
  // Start portal after 60 seconds offline
  // ----------------------------------------------------------

  if (!connected &&
      !testing &&
      now - disconnectedSince >= 60000) {
    startPortal();
  }

  // ----------------------------------------------------------
  // Close portal only after successful setup
  // ----------------------------------------------------------

  if (portalActive &&
      connected &&
      !testing &&
      closePortalAt &&
      int32_t(now - closePortalAt) >= 0) {

    Serial.println(
        "[SETUP] New Wi-Fi active, closing portal");

    stopPortal();
  }

  // Important:
  // Do not automatically close the portal just because
  // the device reconnected to its PREVIOUS network.
  // A failed setup must remain available for retry.

  // ----------------------------------------------------------
  // Connection logs
  // ----------------------------------------------------------

  if (connected && !wasConnected) {
    Serial.printf("[WIFI] connected: ch%d, %d dBm, IP %s, gateway %s, DNS %s\n",
                  WiFi.channel(), WiFi.RSSI(), WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
    connectedAt = now;
    joinFailures.store(0);

    if (!ntpSynced.load()) {
      Serial.println(
          "[NTP] waiting for UTC synchronization");
    }
  }

  if (!connected && wasConnected) {
    Serial.println("[WIFI] connection lost");
  }

  wasConnected = connected;

  // ----------------------------------------------------------
  // Repeated join failures: rescan to name the cause (band, security, password)
  // ----------------------------------------------------------

  if (!connected &&
      !testing &&
      !connectPending &&
      ssid.length() &&
      joinFailures.load() >= 3 &&
      now - lastScanAt >= 120000) {
    lastScanAt = now;
    joinFailures.store(0);
    Serial.printf("[WIFI] repeated failures, last reason %u; diagnosing\n",
                  unsigned(lastReason.load()));
    netdiag::scanFor(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    lastAttempt = now;
  }

  // ----------------------------------------------------------
  // Reconnect previously configured Wi-Fi
  // ----------------------------------------------------------

  if (!connected &&
      !testing &&
      !connectPending &&
      ssid.length() &&
      now - lastAttempt >= 15000) {

    lastAttempt = now;

    Serial.println("[WIFI] reconnecting");

    WiFi.reconnect();
  }

  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  // NTP is UDP 123; cafe/guest networks often block it. TLS needs the
  // real date, so fall back to the Date header of a plain HTTP response.
  if (connected &&
      !ntpSynced.load() &&
      now - connectedAt >= 20000 &&
      now - lastHttpTimeAt >= 30000) {
    lastHttpTimeAt = now;
    Serial.println("[NTP] no reply after 20 s (UDP 123 blocked?); trying HTTP Date");
    if (netdiag::timeFromHttpDate(webHost().c_str())) ntpSynced.store(true);
  }

  if (ready() && !wasSynced) {
    wasSynced = true;

    Serial.printf(
        "[NTP] synchronized UTC %s\n",
        utcNow().c_str());

    if (!probed) {
      // One reachability report per boot: tells a blocked port apart from
      // TLS/credential problems before the first visit arrives.
      probed = true;
      netdiag::probe("broker", MQTT_HOST, MQTT_PORT);
      netdiag::probe("web", webHost().c_str(), 443);
      netdiag::probe("storage", UPLOAD_HOST, 443);
    }
  }

  if (!connected) {
    wasSynced = false;
  }

  return ready();
}

// ============================================================
// Network Ready
// ============================================================

bool WiFiManager::ready() const {
  return WiFi.status() == WL_CONNECTED &&
         ntpSynced.load() &&
         time(nullptr) > 1735689600;
}

