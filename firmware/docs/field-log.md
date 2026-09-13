# Field log

Running record of site visits and deployment state. Newest entry at the top.
Add an entry for every visit and every release, even if the outcome is unknown.

## 2026-09-13 — status check (no visit)

- Reconstructed state from the repo on the desktop laptop. No record of the 2026-08-27
  visit had reached this machine because Claude session data does not sync via Dropbox.
- Added this log and a `CLAUDE.md` so outcomes are written into the repo from now on.
- Moved the project from `Dropbox\Arduino\ChurchWaterDrainMonito` to
  `Dropbox\Sparks Analytics\Projects\ChurchDrainMonitor`, folding in the 2025 sketch and
  the drain 3D models. A signpost README was left at the old path.
- Session history from the other laptop is now shared via Dropbox. The 08-27 through
  08-30 entries below were reconstructed from it.

## 2026-08-30 — hotspot drive-by, backfill confirmed

- Turned on the phone hotspot outside the kitchen. The unit flushed 358 readings covering
  Fri 08-28 00:00 to Sun 08-30 11:35 EDT at exactly 6 per hour, zero gaps.
- Health: fw=5, reset reasons only sw-restart (the OTA) and power-on, no brownout, no panic.
  21 of 21 pings kept on every reading, distance stable within 0.3 cm. Bin dry.
- Established that `SJC Parish Guest` is 5 GHz only. The ESP32 can never join it, so the
  unit is a walk-up logger with no remote alerting until a 2.4 GHz SSID exists.
- Buffer headroom is about 132 days at the calm cadence, so visits are not time-pressured.
- **Open:** ask the parish to enable 2.4 GHz on the AP; drop the dead guest SSID from
  `secrets.h` on the next reflash; rotate the InfluxDB write token; verify depth accuracy
  in the real drain at known fill levels.

## 2026-08-28 — morning after

- The last near-overflow tests from the evening were missing in Grafana. Two causes found:
  the unit was on laptop USB, which browns out the radio while readings keep landing on
  flash, and it was rebooted several times. The firmware keeps only one NTP session anchor,
  so readings from a boot that never reached NTP can never be dated and are skipped on the
  InfluxDB path. They remain in `/log.csv` with the uptime column filled.
- Not a device fault. The single-session anchor is a candidate fix for v6.

## 2026-08-27 — site visit, first v5 install (done)

- On site with the phone hotspot from about 17:52 EDT. Went for the OTA without USB first,
  then attached USB for serial checks.
- v3 to v5 OTA landed. It took about 20 extra minutes: closing and reopening the COM port
  resets the ESP32 and aborts the download, and a failed OTA check waits 6 h before
  retrying (power-cycle to force it).
- First v5 boot printed LittleFS "mount failed", then formatted and remounted. Benign.
- Grafana showed fw=5 and live data. Alarm verified by lifting the float: LED bar, lamp
  and buzzer all worked.
- Power-on session started 18:39 EDT and has run unbroken since.

## 2026-07-28 — firmware v5 released

- Store-and-forward logging to LittleFS, backlog upload to InfluxDB with back-dating,
  on-device `/log.csv`, safe mode after three crashes.
- Manifest flipped to fw-v5. Never run on hardware at release time.
- Server-side items left open: InfluxDB retention on the `drain` bucket, Grafana alert
  rules reacting to backfilled data.

## 2026-07-27 — site visit

- Unit confirmed on v3 with the phone hotspot; the hotspot SSID is in `secrets.h`.

## 2026-07-23 — firmware v3 released

- Re-arm NTP on WiFi reconnect; report SSID in telemetry.
