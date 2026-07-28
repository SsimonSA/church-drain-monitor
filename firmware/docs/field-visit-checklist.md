# Field visit checklist — first v5 install

**State as of 2026-07-28:** firmware **v5 is published and waiting**. The manifest points
at it, the binary is up, but *no device has ever run it*. The field unit is still on
**v3** and will stay there until someone stands next to it with a hotspot.

- Release: https://github.com/SsimonSA/church-drain-monitor/releases/tag/fw-v5
- Commits: `66a1f9e` (firmware), `8ab96ea` (manifest flip)
- Never executed on hardware. It compiles (80.7% flash, 32% RAM) and was reviewed
  closely, but the LittleFS mount, the web server and the session-anchor logic are
  all first-run on this visit.

## Bring

- **Laptop + USB cable.** This is the only rollback. A unit already running v5 has a
  higher `FW_VERSION` than any older release, so it will not downgrade over the air —
  see [../ota/README.md](../ota/README.md).
- A phone with hotspot (the SSID must already be in `secrets.h` — it is, that's how the
  2026-07-27 visit worked).

## What should happen

1. Hotspot on → unit associates, NTP syncs, live data appears in Grafana.
2. **The OTA check trips almost immediately.** Its 6-hour timer has been running dry for
   weeks, so the check fires on the first sample tick after association. Expect a reboot
   within a minute or two — *this is not a crash.*
3. First v5 boot **formats the spiffs partition** (never used before). Adds a few seconds
   to that one boot.
4. It comes up on v5 and starts logging to flash.

The few minutes of v3 data sitting in RAM are lost in that reboot. Expected, not a fault.

## Verify before leaving

- [ ] `http://drain.local/` shows **Firmware v5**. If mDNS won't resolve from the phone,
      use the IP — Grafana now carries it as the `ip` field on the `depth` measurement.
- [ ] That page's **"Log on flash"** figure is growing (it's `log0.bin + log1.bin` bytes).
- [ ] `http://drain.local/log.csv` downloads and opens as a real CSV.
- [ ] Grafana `fw` field flips to `5` and the unit keeps reporting.
- [ ] **Local alarm still works** — this is the part that actually matters. Lift the float
      by hand and confirm the LED bar climbs, the lamp blinks and the buzzer sounds.

## Serial log — what to look at

Every tick prints, among other things:

```
pend=N(flash|RAM)   readings not yet delivered, and where the queue lives
clk=set|unset       whether this boot has an epoch anchor yet
```

Plus at boot: `LOG: mounted — session=N active=… read=… off=…`.

Between them these localize a problem fast:

| Symptom | Reading |
|---|---|
| `pend` climbing, `clk=unset` | Logging fine, no anchor — NTP isn't landing. |
| `pend` climbing, `clk=set`, WiFi up | Delivery failing. Look for the `FLUSH: HTTP …` line — a 4xx here is usually InfluxDB retention, not the network. |
| `pend=0(RAM)` | LittleFS didn't mount; running the RAM fallback. |
| `SAFE MODE` at boot | 3 firmware crashes — filesystem and web server disabled, alarm still running. Needs a USB reflash. |

## If it goes wrong

The unit is designed to fail *toward* being an alarm. In order of severity:

1. **LittleFS won't mount** → falls back to the RAM ring buffer. Telemetry still works,
   history doesn't persist across reboots. Not urgent; drive home.
2. **Crash loop** → after 3 firmware crashes it enters safe mode: no filesystem, no web
   server, sensor/LEDs/lamp/buzzer still running. Confirm the alarm works, then USB
   reflash a known-good build when convenient.
3. **Dead box** → USB reflash on the spot. `pio run -t upload` from `firmware/`.

## Still open (server side, not on the device)

- [ ] **InfluxDB bucket retention on `drain`.** Influx rejects points older than the
      retention window. If it isn't infinite, the first real backlog gets 4xx'd. The
      firmware logs that distinctly but cannot fix it.
- [ ] **Grafana alert rules**, if any exist. Backfilled data is evaluated as it arrives,
      so a backup that ended days ago can fire an alert on delivery.

## Known gap

Readings taken during a boot that *never* reached NTP can't be dated and are never sent
to InfluxDB (they still appear in `/log.csv` with the uptime column filled and epoch
blank). A battery-backed **DS3231** RTC (~$3, I2C) would close this permanently and is
the obvious next hardware addition if it turns out to matter.
