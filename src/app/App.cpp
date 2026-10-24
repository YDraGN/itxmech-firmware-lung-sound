// Production application: task layout and the glue between MQTT and the flow.
//
//   loopTask (core 1, prio 1)   Wi-Fi/NTP/portal + MQTT loop + event publisher
//   worker   (core 1, prio 1)   RecordingFlow: record, upload, complete, retries
//   capture  (core 1, prio 5)   created per recording by audio/Capture
//
// The two long-lived tasks only talk through queues, so a slow upload never
// blocks MQTT keep-alive and a broker outage never blocks the recorder.
// Both are on the task watchdog: a hang becomes a reboot, and the NVS
// checkpoint resumes the visit from the last safe phase.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BuildConfig.h>
#include <atomic>
#include <esp_task_wdt.h>
#include "app/RecordingFlow.h"
#include "core/AppEntry.h"
#include "core/Config.h"
#include "net/MqttLink.h"
#include "net/WifiManager.h"
#include "proto/Protocol.h"

namespace {
struct Assignment { char message[37]; char visit[37]; };
struct Event { char payload[384]; };

QueueHandle_t commands = nullptr;
QueueHandle_t events = nullptr;
TaskHandle_t workerTask = nullptr;
std::atomic<bool> busy{true};
WiFiManager wifi;
MqttLink mqtt;
bool initialized = false;
bool mqttWasConnected = false;
uint32_t lastHealth = 0;

bool networkReady() { return wifi.ready(); }

// Never blocks the worker: if the broker is down long enough to fill the
// queue, the oldest event is dropped (the web reads final state over HTTP).
void emit(const char *type, const String &visitId) {
  Event event{};
  strlcpy(event.payload, eventPayload(type, visitId).c_str(), sizeof(event.payload));
  if (xQueueSend(events, &event, 0) != pdTRUE) {
    Event dropped;
    xQueueReceive(events, &dropped, 0);
    xQueueSend(events, &event, 0);
    Serial.println("[MQTT] event queue full; oldest event dropped");
  }
  Serial.printf("[EVENT] %s\n", type);
}

void onCommand(String &topic, String &payload) {
  if (topic != mqtt.commandTopic() || payload.length() > 1024) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc.is<JsonObject>() ||
      !doc["version"].is<int>() || doc["version"].as<int>() != 1 ||
      doc["type"] != "ASSIGN_VISIT" || !doc["messageId"].is<String>() ||
      !doc["visitId"].is<String>() || !doc["timestamp"].is<String>()) {
    Serial.println("[MQTT] invalid command rejected");
    return;
  }
  String message = doc["messageId"].as<String>();
  String visit = doc["visitId"].as<String>();
  if (!validUuid(message) || !validUuid(visit) ||
      !validTimestamp(doc["timestamp"].as<String>())) {
    Serial.println("[MQTT] invalid identifiers/timestamp");
    return;
  }
  if (busy.load()) {
    // Typically a pending upload: try it right away instead of waiting.
    Serial.println("[MQTT] busy; assignment rejected, current visit unchanged");
    if (workerTask) xTaskNotifyGive(workerTask);
    return;
  }
  message.toLowerCase();
  visit.toLowerCase();
  Assignment assignment{};
  strlcpy(assignment.message, message.c_str(), sizeof(assignment.message));
  strlcpy(assignment.visit, visit.c_str(), sizeof(assignment.visit));
  if (xQueueSend(commands, &assignment, 0) == pdTRUE) {
    busy = true;
    Serial.println("[MQTT] ASSIGN_VISIT queued");
  }
}

void worker(void *) {
  esp_task_wdt_add(nullptr);
  static RecordingFlow flow(emit, networkReady);
  if (!flow.begin()) {
    Serial.println("[BOOT] storage/audio unavailable; recorder disabled");
    workerTask = nullptr;  // assignments stay rejected (busy)
    esp_task_wdt_delete(nullptr);
    vTaskDelete(nullptr);
    return;
  }
  flow.resume();
  busy = !flow.idle();
  Assignment assignment;
  for (;;) {
    esp_task_wdt_reset();
    if (xQueueReceive(commands, &assignment, pdMS_TO_TICKS(1000)) == pdTRUE)
      flow.assign(assignment.message, assignment.visit);
    flow.service(ulTaskNotifyTake(pdTRUE, 0) > 0);
    busy = !flow.idle() || uxQueueMessagesWaiting(commands) > 0;
  }
}

void logHealth() {
  if (millis() - lastHealth < 60000) return;
  lastHealth = millis();
  Serial.printf("[HEALTH] up %lus heap %u (min %u, block %u) wifi=%d mqtt=%d busy=%d\n",
                millis() / 1000, unsigned(ESP.getFreeHeap()), unsigned(ESP.getMinFreeHeap()),
                unsigned(ESP.getMaxAllocHeap()), int(wifi.ready()), int(mqtt.connected()),
                int(busy.load()));
}
}  // namespace

void appSetup() {
  Serial.begin(921600);
  Serial.printf("\n[BOOT] lung-sound recorder, device %s, reset reason %d\n", DEVICE_ID,
                int(esp_reset_reason()));
  esp_task_wdt_init(cfg::kTaskWatchdogS, true);
  commands = xQueueCreate(1, sizeof(Assignment));
  events = xQueueCreate(16, sizeof(Event));
  if (!commands || !events) {
    Serial.println("[BOOT] queue allocation failed");
    return;
  }
  wifi.begin();
  mqtt.begin(onCommand);
  if (xTaskCreatePinnedToCore(worker, "worker", 16384, nullptr, 1, &workerTask, 1) != pdPASS) {
    Serial.println("[BOOT] worker allocation failed");
    return;
  }
  enableLoopWDT();
  initialized = true;
}

void appLoop() {
  if (!initialized) {
    delay(1000);
    return;
  }
  mqtt.tick(wifi.tick());
  bool connected = mqtt.connected();
  // Broker back means the network works again: retry a pending upload now.
  if (connected && !mqttWasConnected && workerTask) xTaskNotifyGive(workerTask);
  mqttWasConnected = connected;

  Event event;
  if (xQueuePeek(events, &event, 0) == pdTRUE && mqtt.publishEvent(event.payload))
    xQueueReceive(events, &event, 0);
  logHealth();
  delay(5);
}
