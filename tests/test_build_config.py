"""scripts/build_config.py without toolchains or real secrets."""
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_config.py"


class FakeEnvironment:
    def __init__(self, root, mode):
        self.root, self.mode, self.filter = root, mode, None

    def subst(self, value):
        return self.mode if value == "$PIOENV" else str(self.root)

    def Replace(self, **values):
        self.filter = values["SRC_FILTER"]

    def Append(self, **values):
        pass


class BuildConfigTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="lungsound-config-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        (self.root / "respicare-mqtt-ca.crt").write_text(
            "-----BEGIN CERTIFICATE-----\nTEST_ONLY\n-----END CERTIFICATE-----")
        self.values = {k: "test-only" for k in (
            "DEVICE_ID", "MQTT_HOST", "MQTT_USERNAME", "MQTT_PASSWORD",
            "FIRMWARE_DEVICE_API_KEY")}
        self.values.update(WEB_BASE_URL="https://backend.example/",
                           UPLOAD_HOST="storage.example")

    def run_hook(self, mode, values):
        env = FakeEnvironment(self.root, mode)
        with patch.dict(os.environ, values, clear=True):
            runpy.run_path(str(SCRIPT), init_globals={"env": env, "Import": lambda _: None})
        return env

    def header(self):
        return (self.root / "include/BuildConfig.h").read_text()

    def test_production_header_and_sources(self):
        env = self.run_hook("production", self.values)
        header = self.header()
        self.assertIn("#define APP_MODE_PRODUCTION 1", header)
        self.assertIn('#define WEB_BASE_URL "https://backend.example"', header)
        self.assertIn("#define RECORD_SECONDS 20", header)
        self.assertIn('#define WIFI_SSID ""', header)  # portal provisioning allowed
        self.assertIn("+<net/>", env.filter)
        self.assertNotIn("+<debug/>", env.filter)

    def test_missing_secret_fails_before_writing(self):
        del self.values["MQTT_PASSWORD"]
        with self.assertRaisesRegex(RuntimeError, "MQTT_PASSWORD"):
            self.run_hook("production", self.values)
        self.assertFalse((self.root / "include/BuildConfig.h").exists())

    def test_placeholder_rejected(self):
        self.values["FIRMWARE_DEVICE_API_KEY"] = "REPLACE_ME"
        with self.assertRaisesRegex(RuntimeError, "FIRMWARE_DEVICE_API_KEY"):
            self.run_hook("production", self.values)

    def test_http_base_url_rejected(self):
        self.values["WEB_BASE_URL"] = "http://backend.example"
        with self.assertRaisesRegex(RuntimeError, "HTTPS"):
            self.run_hook("production", self.values)

    def test_record_seconds_must_fit_flash(self):
        self.values["RECORD_SECONDS"] = "45"
        with self.assertRaisesRegex(RuntimeError, "RECORD_SECONDS"):
            self.run_hook("production", self.values)

    def test_private_key_rejected(self):
        (self.root / "respicare-mqtt-ca.crt").write_text("-----BEGIN PRIVATE KEY-----")
        with self.assertRaisesRegex(RuntimeError, "public CA"):
            self.run_hook("production", self.values)

    def test_serial_stream_needs_no_secrets(self):
        env = self.run_hook("stream_serial", {})
        self.assertEqual(self.header().count("#define"), 4)  # mode + 3 MIC_* values
        self.assertNotIn("MQTT", self.header())
        self.assertIn("+<debug/>", env.filter)
        self.assertNotIn("+<net/>", env.filter)

    def test_udp_stream_needs_wifi_and_ipv4(self):
        with self.assertRaisesRegex(RuntimeError, "UDP_HOST"):
            self.run_hook("stream_udp", {"WIFI_SSID": "bench"})
        with self.assertRaisesRegex(RuntimeError, "IPv4"):
            self.run_hook("stream_udp", {"WIFI_SSID": "bench", "UDP_HOST": "pc.local"})
        self.run_hook("stream_udp", {"WIFI_SSID": "bench", "UDP_HOST": "172.16.0.254"})
        self.assertIn('#define UDP_HOST "172.16.0.254"', self.header())
        self.assertIn("#define UDP_PORT 12345", self.header())
        self.assertNotIn("MQTT_PASSWORD", self.header())

    def test_dotenv_loaded_for_ide_builds_and_shell_wins(self):
        lines = [f"{k}={v}" for k, v in self.values.items() if k != "DEVICE_ID"]
        lines += ["# comment", "export DEVICE_ID='FROM-DOTENV'",
                  "WIFI_SSID='Cafe & Co 2.4G'", 'WIFI_PASSWORD="test-only-pass"']
        (self.root / ".env").write_text("\n".join(lines))
        self.run_hook("production", {"MQTT_HOST": "shell.example"})
        header = self.header()
        self.assertIn('#define DEVICE_ID "FROM-DOTENV"', header)
        self.assertIn('#define WIFI_SSID "Cafe & Co 2.4G"', header)
        self.assertIn('#define MQTT_HOST "shell.example"', header)

    def test_mic_dsp_defaults_and_validation(self):
        self.run_hook("stream_serial", {})
        header = self.header()
        self.assertIn("#define MIC_GAIN 2.0", header)
        self.assertIn("#define MIC_HPF_HZ 20.0", header)
        self.assertIn("#define MIC_LPF_HZ 2000.0", header)
        self.run_hook("stream_serial", {"MIC_GAIN": "1.5", "MIC_HPF_HZ": "100"})
        self.assertIn("#define MIC_GAIN 1.5", self.header())
        with self.assertRaisesRegex(RuntimeError, "MIC_GAIN"):
            self.run_hook("stream_serial", {"MIC_GAIN": "loud"})
        with self.assertRaisesRegex(RuntimeError, "MIC_HPF_HZ"):
            self.run_hook("stream_serial", {"MIC_HPF_HZ": "5"})
        with self.assertRaisesRegex(RuntimeError, "4x"):
            self.run_hook("stream_serial", {"MIC_HPF_HZ": "300", "MIC_LPF_HZ": "1000"})

    def test_unknown_environment_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unknown environment"):
            self.run_hook("esp32dev", self.values)


if __name__ == "__main__":
    unittest.main()
