#pragma once
// MQTT over TLS. Topics (unchanged contract with the MQTT gateway):
//   respicare/device/<DEVICE_ID>/command  <- ASSIGN_VISIT (QoS1)
//   respicare/device/<DEVICE_ID>/event    -> flow events (QoS1)
//   respicare/device/<DEVICE_ID>/status   -> DEVICE_ONLINE / DEVICE_OFFLINE (retained LWT)
#include <Arduino.h>
#include <MQTT.h>
#include <WiFiClientSecure.h>

using CommandHandler = void (*)(String &topic, String &payload);

class MqttLink {
 public:
  void begin(CommandHandler handler);
  void tick(bool networkReady);
  bool connected() { return client_.connected(); }
  bool publishEvent(const char *payload);
  String commandTopic() const { return topic("command"); }

 private:
  WiFiClientSecure tls_;
  MQTTClient client_{2048};
  uint32_t nextAttempt_ = 0;
  unsigned failures_ = 0;
  String topic(const char *suffix) const;
};
