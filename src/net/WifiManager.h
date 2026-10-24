#pragma once
// Wi-Fi station + NTP + captive setup portal (RespiCare-Setup-<id>), carried
// over unchanged from itxmech-respiratory-firmware.
#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

class WiFiManager {
 public:
  void begin();
  bool tick();         // call from the network task; returns ready()
  bool ready() const;  // associated AND UTC time synchronised (needed for TLS)

 private:
  uint32_t lastAttempt = 0;
  bool wasConnected = false;
  bool wasSynced = false;

  WebServer portal{80};
  DNSServer dns;
  Preferences settings;

  String ssid;
  String password;
  String pendingSsid;
  String pendingPassword;

  bool portalActive = false;
  bool testing = false;
  bool connectPending = false;

  uint32_t disconnectedSince = 0;
  uint32_t testStarted = 0;
  uint32_t closePortalAt = 0;

  String portalMessage;

  void startPortal();
  void stopPortal();
  void showPortal();
  void redirectToPortal();
};
