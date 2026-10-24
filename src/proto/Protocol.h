#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

bool validUuid(const String &value);
bool validTimestamp(const String &value);
String utcNow();
String eventPayload(const char *type, const String &visitId = "");
