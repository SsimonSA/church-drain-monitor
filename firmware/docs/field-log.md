# Field log

Running record of site visits and deployment state. Newest entry at the top.
Add an entry for every visit and every release, even if the outcome is unknown.

## 2026-09-13 — firmware v6 built (flush-before-NTP fix), release pending

- flushChunk() now stops at the first record from the current boot while NTP is still
  pending, so a reboot no longer discards its own backlog from the InfluxDB path.
- Sensing, thresholds and the on-flash record format are unchanged; the stored log
  survives the update.
- Built and committed. **Release fw-v6 and the manifest flip still to do** (see
  `ota/README.md`); until the manifest says 6 the unit stays on v5.
- Also dropped the unjoinable `SJC Parish Guest` entry from `secrets.h` (5 GHz only) so
  the unit stops burning a 10 s timeout on it each scan cycle. Baked into the v6 binary.
- The readings from 09-01 to 09-13 are still only on the device. Pull `/log.csv` on the
  next visit before the OTA reboot if you want them.

## 2026-09-13 — hotspot drive-by, backlog stops at 09-01

- Hotspot on outside the kitchen. The unit connected and delivered its backlog, but only
  from Sun 08-30 11:45 through Tue 09-01 12:12 EDT (598 readings, hotspot IP 10.73.185.210,
  a different address from the 08-30 flush, so this was today's delivery). Nothing newer
  arrived, and no live readings from today either.
- One real event in that window: Tue 09-01 08:57:22 to 08:57:30, a 9-second excursion
  peaking at 4.82" (raw jumped, slew limiter ramped 1.5, 3.0, 4.5, 4.82, back down).
  Enough to trip the 4.5" fast chirp for a few seconds. Then the 1 Hz settle window and a
  return to calm 10-minute heartbeats until 12:12.
- **Diagnosis (from the firmware, not yet confirmed on the device):** the unit rebooted
  around 12:15 on 09-01 (today's rows carry rst=1, a clean power-on). The new boot had no
  NTP until today. On connect, flushChunk() runs before NTP lands, dates the old session's
  records via the saved anchor, but treats the new session's own pre-NTP records as
  undatable and advances the delivery cursor past them. Seconds later NTP arrives, too late.
  Twelve days of readings are still on flash and are now datable for `/log.csv`, but the
  InfluxDB path has skipped them for good.
- No live point from today because calm mode enqueues only every 10 min and the visit was
  shorter than that.
- **Next visit:** stay on the hotspot 10+ min, download `http://<ip>/log.csv` (the ip field
  shows the address), and read the serial boot line `LOG: mounted — session=N` to confirm
  the session count went up. Ask whether anyone unplugged the box around noon on 09-01.
- **v6 fix:** in flushChunk(), when g_bootEpoch is still 0, stop the chunk at the first
  record from the current session instead of skipping it, so the backlog waits for NTP.

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
