# 🚰 Church Water Drain Monitor

An ESP32-based early-warning system for a church kitchen floor drain that backs up
under a wash sink. A float rides in a 2" PVC standpipe; an HC-SR04 ultrasonic sensor
reads the float height down the bore, driving a 5-LED level bar, a blinking overflow
lamp, and a piezo alarm — and posting depth telemetry to InfluxDB for a Grafana
dashboard. The box is a self-contained alarm first, a cloud sensor second.

See [PROJECT.md](PROJECT.md) for the full design history (sensing approaches tried,
mechanical build, calibration).

## Getting the data off the box

The site has no dependable WiFi, so the firmware assumes it is usually offline and
logs regardless:

- **Store and forward.** Readings go to a persistent log in flash (LittleFS, two
  rotating files, ~37,000 readings ≈ 12 h of continuous 1 Hz logging or ~8 months of
  calm heartbeats). It survives reboots, brownouts and power cuts. When a network
  appears, the backlog is uploaded oldest-first in bounded chunks and back-dated to
  when it was actually measured, so Grafana fills in the past with no dashboard
  changes.
- **Local download.** The device also serves its own history over plain HTTP on the
  LAN — park next to it, turn on a phone hotspot, and open
  **`http://drain.local/log.csv`** (or `http://<ip>/log.csv`; the IP is reported in
  telemetry as the `ip` field). This path needs no InfluxDB, no token, no retention
  window and no internet.

Readings are stamped with **uptime**, not wall-clock, so logging never depends on the
device having had a clock; absolute time is applied on the way out once NTP lands.
Readings taken during a boot that never reached NTP keep their shape in the CSV
(uptime column) but are deliberately **not** sent to InfluxDB — a guessed timestamp
would corrupt the dashboard. A battery-backed RTC (DS3231) would close that gap.

## Repository layout

| Path | What it is |
|------|------------|
| `firmware/` | **Field firmware** (`StJoesDrainMonitor`) — the deployed build: sensor, LEDs, buzzer, button, InfluxDB telemetry. |
| `experiments/BenchTest/` | Interactive bench harness — exercise LEDs / buzzer / sensor / WiFi / InfluxDB write from a serial menu. |
| `experiments/BtnLedTest/` | Minimal button + LED sanity sketch. |
| `experiments/vl53l1x-abandoned/` | Abandoned VL53L1X ToF version (kept for reference). |
| `experiments/ultrasonic-original-2025/` | The very first sketch (bare HC-SR04, six LEDs), archived from the old sketchbook. |
| `hardware/3d-models/` | Copies of the printable parts (float, tube liner, sensor cap). |

## Setup — credentials

Credentials are **not** committed. Each firmware project reads them from a gitignored
`secrets.h`. To build:

```sh
cd firmware/src   # (and likewise experiments/BenchTest/src)
cp secrets.example.h secrets.h
# then edit secrets.h with your WiFi network(s) and InfluxDB write token
```

`secrets.h` provides `WIFI_NETS[]`, `N_WIFI`, and `INFLUX_TOKEN`. Use a **write-only,
bucket-scoped** InfluxDB token — that token is embedded in the compiled binary.

## Field visits

The unit is offline except when someone is standing next to it with a phone hotspot, so
an update and its first test happen on the same trip.
**[firmware/docs/field-visit-checklist.md](firmware/docs/field-visit-checklist.md)** is
the pre-flight for that: what to bring, what should happen, what to verify before
leaving, and how to read the serial log if it doesn't.

## Over-the-air updates

The field firmware self-updates over the internet — the device polls a manifest and
flashes a newer binary from a GitHub release when the water is low. There is no push
from the IDE. Full process and the publish/rollback steps are in
[firmware/ota/README.md](firmware/ota/README.md); the live manifest is
[firmware/ota/manifest.txt](firmware/ota/manifest.txt).

## Toolchain

PlatformIO (`espressif32` / `esp32dev`). Open a project folder and `pio run -t upload`.
