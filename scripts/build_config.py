"""Generate include/BuildConfig.h from the build environment.

The PlatformIO environment name selects the application:
  production    - MQTT + recording + HTTPS upload (needs every secret)
  stream_serial - PCM over USB serial, no network and no secrets
  stream_udp    - PCM over UDP to a bench PC, needs Wi-Fi + UDP target only
"""
import ipaddress
import json
import os
from pathlib import Path
import re

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
mode = env.subst("$PIOENV")


def load_dotenv(path):
    """Fill missing variables from .env so VS Code's Build/Upload buttons work.

    Accepts KEY=VALUE or export KEY=VALUE, optional single/double quotes.
    Variables already exported in the shell win. Values are never printed.
    """
    if not path.is_file():
        return 0
    loaded = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*", line)
        if not match or line.lstrip().startswith("#"):
            continue
        key, value = match.groups()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        if key not in os.environ:
            os.environ[key] = value
            loaded += 1
    return loaded


loaded = load_dotenv(root / ".env")
if loaded:
    print(f"build_config: {loaded} variable(s) loaded from .env")

COMMON = ["+<*.cpp>", "+<dsp/>", "+<hal/>", "+<audio/>", "+<core/>"]
SOURCES = {
    "production": COMMON + ["+<app/>", "+<net/>", "+<storage/>", "+<proto/>"],
    "stream_serial": COMMON + ["+<debug/>"],
    "stream_udp": COMMON + ["+<debug/>"],
}


def required(name):
    value = os.environ.get(name, "")
    if not value or "REPLACE" in value or "YOUR_" in value or "<" in value:
        raise RuntimeError(f"Missing or placeholder environment variable: {name}")
    return value


def certificate(path):
    value = path.read_text()
    if "-----BEGIN CERTIFICATE-----" not in value or "PRIVATE KEY" in value:
        raise RuntimeError(f"{path.name} must contain public CA certificates only")
    return value


def record_seconds():
    value = int(os.environ.get("RECORD_SECONDS", "20"))
    # 16 kHz mono 16-bit; keep one WAV well inside the ~1.3 MiB LittleFS slot.
    if not 5 <= value <= 30:
        raise RuntimeError("RECORD_SECONDS must be between 5 and 30")
    return value


if mode not in SOURCES:
    raise RuntimeError(f"Unknown environment {mode}; expected one of {sorted(SOURCES)}")

def number(name, default, low, high):
    raw = os.environ.get(name, "") or str(default)
    try:
        value = float(raw)
    except ValueError as error:
        raise RuntimeError(f"{name} must be a number") from error
    if not low <= value <= high:
        raise RuntimeError(f"{name} must be between {low} and {high}")
    return value


values = {"APP_MODE_" + mode.upper(): 1}
# Microphone DSP, tunable per hardware without code changes (all modes).
# Direct skin contact on the INMP441 port is ~40 dB hotter than through the
# stethoscope tube, hence the low floor.
values["MIC_GAIN"] = number("MIC_GAIN", 2.0, 0.05, 32.0)
values["MIC_HPF_HZ"] = number("MIC_HPF_HZ", 20.0, 10.0, 300.0)
values["MIC_LPF_HZ"] = number("MIC_LPF_HZ", 2000.0, 800.0, 7000.0)
if values["MIC_LPF_HZ"] < 4 * values["MIC_HPF_HZ"]:
    raise RuntimeError("MIC_LPF_HZ must be at least 4x MIC_HPF_HZ")
if mode == "production":
    values["WIFI_SSID"] = os.environ.get("WIFI_SSID", "")
    values["WIFI_PASSWORD"] = os.environ.get("WIFI_PASSWORD", "")
    if values["WIFI_SSID"]:
        values["WIFI_SSID"] = required("WIFI_SSID")
        if values["WIFI_PASSWORD"]:
            values["WIFI_PASSWORD"] = required("WIFI_PASSWORD")
    for key in ("DEVICE_ID", "MQTT_HOST", "MQTT_USERNAME", "MQTT_PASSWORD",
                "WEB_BASE_URL", "FIRMWARE_DEVICE_API_KEY", "UPLOAD_HOST"):
        values[key] = required(key)
    if not values["WEB_BASE_URL"].startswith("https://"):
        raise RuntimeError("WEB_BASE_URL must use HTTPS")
    values["WEB_BASE_URL"] = values["WEB_BASE_URL"].rstrip("/")
    values["MQTT_PORT"] = int(os.environ.get("MQTT_PORT", "8883"))
    if not 1 <= values["MQTT_PORT"] <= 65535:
        raise RuntimeError("MQTT_PORT must be a valid TCP port")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", values["DEVICE_ID"]):
        raise RuntimeError("DEVICE_ID must be a short topic-safe identifier")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", values["UPLOAD_HOST"]):
        raise RuntimeError("UPLOAD_HOST must be an exact DNS hostname")
    values["RECORD_SECONDS"] = record_seconds()
    values["MQTT_CA"] = certificate(root / "respicare-mqtt-ca.crt")
elif mode == "stream_udp":
    values["WIFI_SSID"] = required("WIFI_SSID")
    values["WIFI_PASSWORD"] = os.environ.get("WIFI_PASSWORD", "")
    host = required("UDP_HOST")
    try:
        ipaddress.IPv4Address(host)
    except ValueError as error:
        raise RuntimeError("UDP_HOST must be the bench PC IPv4 address") from error
    values["UDP_HOST"] = host
    values["UDP_PORT"] = int(os.environ.get("UDP_PORT", "12345"))
    if not 1 <= values["UDP_PORT"] <= 65535:
        raise RuntimeError("UDP_PORT must be a valid UDP port")

env.Replace(SRC_FILTER=SOURCES[mode])
target = root / "include" / "BuildConfig.h"
target.parent.mkdir(exist_ok=True)
target.write_text("// Generated from environment. Never commit this file.\n#pragma once\n" +
                  "\n".join(f"#define {k} {json.dumps(v)}" for k, v in values.items()) + "\n")
env.Append(CPPPATH=[str(target.parent)])
