#include "net/MqttLink.h"
#include <BuildConfig.h>
#include "core/Config.h"
#include "core/RetryPolicy.h"
#include "net/HttpUploader.h"
#include "net/NetDiagnostics.h"
#include "proto/Protocol.h"

String MqttLink::topic(const char *suffix) const {
  return String("respicare/device/") + DEVICE_ID + "/" + suffix;
}

void MqttLink::begin(CommandHandler handler) {
  tls_.setCACert(MQTT_CA);
  tls_.setHandshakeTimeout(8);
  client_.begin(MQTT_HOST, MQTT_PORT, tls_);
  // keepalive 30 s, persistent session off, 1.5 s socket timeout per call.
  client_.setOptions(30, false, 1500);
  client_.onMessage(handler);
}

void MqttLink::tick(bool networkReady) {
  if (!networkReady) return;
  if (client_.connected()) {
    client_.loop();
    return;
  }
  if (int32_t(millis() - nextAttempt_) < 0) return;
  if (!HttpUploader::heapOkForTls()) {
    Serial.printf("[MQTT] postponed: heap free %u / block %u too small for TLS\n",
                  unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()));
    nextAttempt_ = millis() + 5000;
    return;
  }

  Serial.println("[MQTT] connecting TLS");
  String offline = eventPayload("DEVICE_OFFLINE");
  client_.setWill(topic("status").c_str(), offline.c_str(), true, 1);
  if (client_.connect(DEVICE_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    String online = eventPayload("DEVICE_ONLINE");
    if (client_.subscribe(commandTopic(), 1) &&
        client_.publish(topic("status"), online, true, 1)) {
      failures_ = 0;
      Serial.println("[MQTT] connected; command subscribed QoS1; DEVICE_ONLINE retained");
      return;
    }
    client_.disconnect();
  }
  Serial.printf("[MQTT] connect failed (error=%d, connack=%d)\n",
                int(client_.lastError()), int(client_.returnCode()));
  // connack 4/5 = bad username/password or not authorised (broker reached).
  if (failures_ % 5 == 2 && netdiag::probe("broker", MQTT_HOST, MQTT_PORT))
    Serial.println("[MQTT] port reachable: check MQTT CA file, username/password and broker ACL");
  nextAttempt_ = millis() + backoffMs(min(failures_++, 5U), 1000, 32000, esp_random() % 500);
}

bool MqttLink::publishEvent(const char *payload) {
  return client_.connected() && client_.publish(topic("event"), payload, false, 1);
}
