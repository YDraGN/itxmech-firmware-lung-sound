#include "proto/Protocol.h"
#include <BuildConfig.h>
#include <time.h>

bool validUuid(const String &v) {
  if (v.length() != 36) return false;
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (v[i] != '-') return false;
    } else if (!isxdigit((unsigned char)v[i])) return false;
  }
  return true;
}

bool validTimestamp(const String &v) {
  if (v.length() != 20 && v.length() != 24) return false;
  if (v[4] != '-' || v[7] != '-' || v[10] != 'T' || v[13] != ':' ||
      v[16] != ':' || v[v.length() - 1] != 'Z') return false;
  for (unsigned i = 0; i < v.length() - 1; ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
    if (i == 19 && v.length() == 24) { if (v[i] != '.') return false; }
    else if (!isdigit((unsigned char)v[i])) return false;
  }
  int y = v.substring(0, 4).toInt(), m = v.substring(5, 7).toInt();
  int d = v.substring(8, 10).toInt();
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1970 || m < 1 || m > 12 || d < 1) return false;
  int maxDay = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  return d <= maxDay && v.substring(11, 13).toInt() < 24 &&
         v.substring(14, 16).toInt() < 60 && v.substring(17, 19).toInt() < 60;
}

String utcNow() {
  time_t now = time(nullptr);
  struct tm value;
  gmtime_r(&now, &value);
  char buf[21];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &value);
  return String(buf);
}

String eventPayload(const char *type, const String &visitId) {
  JsonDocument doc;
  doc["version"] = 1;
  doc["type"] = type;
  doc["deviceId"] = DEVICE_ID;
  if (!visitId.isEmpty()) doc["visitId"] = visitId;
  doc["timestamp"] = utcNow();
  String payload;
  serializeJson(doc, payload);
  return payload;
}
