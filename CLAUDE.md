## Security Rules

- **NEVER read, open, inspect, print, or parse `.env` files or files containing environment secrets.**
- Do not use commands such as `cat`, `type`, `Get-Content`, `grep`, or `Select-String` to inspect `.env` files.
- Do not read `.env`, `.env.*`, or equivalent environment files in any directory.
- Do not read `include/BuildConfig.h`: it is generated from those secrets.
- Do not display, copy, log, or expose API keys, access tokens, passwords, database credentials, or other secrets.
- If environment variable names are needed, ask the user to provide the variable names without their values.
- If a task requires environment configuration, use `.env.example` with placeholder values instead.

## Verification

- Host tests (no hardware): `python tests/run_host_tests.py` (needs g++ on PATH).
- The user runs builds/tests themself from VS Code; do not run them unless asked.
- `scripts/build_config.py` auto-loads `.env` (user allowed reading it to check completeness; never print values).
- Compile check with dummy values: export the production variables with `test-only`
  values and run `pio run -e production` (also `-e stream_serial`, and `-e stream_udp`
  with `WIFI_SSID`/`UDP_HOST`). PlatformIO lives in
  `../itxmech-respiratory-firmware/venv/Scripts/pio.exe` on this machine.
- Delete the dummy `include/BuildConfig.h` afterwards.
- Real flashing is done by the hardware team; ask them for serial logs.
