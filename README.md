# 🚰 Church Water Drain Monitor

An ESP32-based early-warning system for a church kitchen floor drain that backs up
under a wash sink. A float rides in a 2" PVC standpipe; an HC-SR04 ultrasonic sensor
reads the float height down the bore, driving a 5-LED level bar, a blinking overflow
lamp, and a piezo alarm — and posting depth telemetry to InfluxDB for a Grafana
dashboard. The box is a self-contained alarm first, a cloud sensor second.

See [PROJECT.md](PROJECT.md) for the full design history (sensing approaches tried,
mechanical build, calibration).

## Repository layout

| Path | What it is |
|------|------------|
| `firmware/` | **Field firmware** (`StJoesDrainMonitor`) — the deployed build: sensor, LEDs, buzzer, button, InfluxDB telemetry. |
| `experiments/BenchTest/` | Interactive bench harness — exercise LEDs / buzzer / sensor / WiFi / InfluxDB write from a serial menu. |
| `experiments/BtnLedTest/` | Minimal button + LED sanity sketch. |
| `experiments/vl53l1x-abandoned/` | Abandoned VL53L1X ToF version (kept for reference). |

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

## Toolchain

PlatformIO (`espressif32` / `esp32dev`). Open a project folder and `pio run -t upload`.
