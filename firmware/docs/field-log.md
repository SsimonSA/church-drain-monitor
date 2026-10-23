# Field log

Running record of site visits and deployment state. Newest entry at the top.
Add an entry for every visit and every release, even if the outcome is unknown.

## 2026-10-22 — fw v10: the alarm dwell was wrong, reverted before it ever shipped

- **v9's 45 s alarm dwell is removed. The buzzer fires the instant the level crosses
  5.0 in, as in v8 and earlier.** The manifest points at v10; v9 should not be deployed.
- The unit never ran v9 — it was still on v7 when this was caught, so the dwell never
  reached hardware.
- **Why the dwell was wrong:** it was reasoned from telemetry alone (discharges clear in
  ~10 s, so filter on persistence rather than peak). The missing fact is physical —
  **a partially clogged drain can go from normal to overflowing in about 20 seconds**, so
  any dwell long enough to filter a sink discharge is long enough for the floor to be wet
  first. Alarm latency must be zero; the box exists to give the person at the sink
  maximum warning. A false alarm about once a fortnight is the accepted price.
- If nuisance alarms ever need addressing: change the **threshold** or the **ack
  behaviour**, never the latency. The source now carries a prominent
  DO-NOT-ADD-A-DWELL block with this rationale, because the v9 conclusion is easy to
  re-derive from the data and wrong every time.
- Retained from v8/v9: `DEADBAND_IN` 1.8 in, hourly OTA checks, 10 min retry after a
  failed check.

## 2026-10-22 — fw v8 and v9 released the same evening

- **v8:** `DEADBAND_IN` 1.4 → 1.8 in, so an empty drain reads a flat 0.00 instead of
  flickering 0.00/1.42 after the remount. Thresholds untouched.
- **v9 (what the unit actually takes — v8 is superseded and skipped):**
  - **`ALARM_DWELL_MS = 45 s`.** The level must hold at/above 5.0 in for 45 s before the
    latching beep-until-ack starts, because routine sink use was latching the 95 dB
    alarm. LED bar, red lamp and the escalating chirps stay instantaneous, and the buzzer
    holds at the WARN2 fast chirp during the dwell. Only dropping below `ALARM_CLEAR_IN`
    (4.8 in) cancels the dwell, so a level oscillating around 5.00 during a real backup
    cannot restart it forever.
  - **OTA cadence 6 h → 1 h**, now that the bridge keeps the unit online.
  - **A failed OTA check retries in 10 min** instead of burning the window.
    `checkForOTA()` returns success and the next attempt is scheduled from the outcome;
    "already up to date" counts as success. Previously one dropped connection cost 6 h
    and the only recovery was a power cycle, which defeats remote updating.
- Releases `fw-v8` and `fw-v9` published, assets verified HTTP 200, manifest says 9.
- **This was the first OTA ever attempted over the bridge rather than a phone hotspot**,
  and the first remote-capable firmware the project has had.

## 2026-10-22 — site visit: remote monitoring achieved; the sensor was never broken

- **The bridge works. No captive portal.** A phone joined to `DrainMonitor` loaded
  `http://neverssl.com` through to the real site (it reached neverssl's own random
  subdomain, so DNS and outbound HTTP both left the building). This closes the question
  that has been open since 08-30: a 2.4 GHz network with real internet now exists at the
  kitchen.
- On first power-up the RE315 still carried the bench config — Tether showed
  **Main Network: No Connection**, so `DrainMonitor` was broadcasting with no uplink and
  the phone reported "connected without internet". Re-paired Main Network to
  `SJC Parish Guest` (open, 5 GHz).
- **Extended-network security had to be set explicitly.** It was on "Same as main
  network", and the guest net has no password, so `DrainMonitor` would have come up
  **open** — and the ESP32 presents a password, which fails against an open AP. Now set
  to WPA/WPA2-Personal with `4109771567`, matching `secrets.h`. 2.4 GHz extended on,
  5 GHz extended off. Anyone re-running the setup wizard must re-check this.
- Extender left plugged in and running.
- **v7 flashed over USB on site** (clean build, 80.7% flash / 32.0% RAM, hash verified).
  The unit then joined `DrainMonitor` by itself, **NTP synced through the church guest
  network** (`Clock: synced`), and InfluxDB delivery went to `Undelivered 0`. The whole
  transport chain is now verified end to end with the real hardware. `OTA: running v7,
  manifest v7 — up to date` also confirms the OTA path works over the bridge.
- The v6/v7 flush fix behaved as designed on first boot: ~6,000 records from the
  unanchored previous session were reported `kept for /log.csv, not sent` rather than
  being silently skipped.
- Signal on the bench table was -80 dBm, then -71 dBm after moving it. **Not yet measured
  at the mount point** — check `http://drain.local/` after remounting.

### The sensor was never broken

- Suspected failed; it is not. Every reading through every symptom showed
  `k=21 ow=0 ne=0` — 21 of 21 pings kept, zero rejects, zero no-echo. A failing SR04
  cannot produce that. Ceiling test: `ow=21 ne=0`, i.e. all pings returned, all beyond
  the 30 cm window. It was correctly ranging whatever was in front of it.
- `/log.csv` (13,835 records back to 2026-08-27) shows session 4 running 10-11 through
  22:00 UTC on 10-22 with a mean raw of 25.09 cm — normal and empty the entire time. The
  phantom values begin at **21:54 UTC (17:54 local)**, the minute the probe was first
  handled, and track the handling: 17.5 cm being moved, 7.64 cm on the table, 23.76 cm
  reassembled.
- **Three real water events** in 11 days, each a clean rise and fall over ~9 s with
  perfect ping quality: 10-14 17:33 UTC peak **6.02 in** (past the 5.5" overflow line —
  this is what left the alarm acknowledged), 10-19 14:30 peak 3.64 in, 10-22 13:14 peak
  4.02 in. A nine-second slug of water, not a blocked drain — a mop bucket emptied into
  the floor drain fits the shape.
### What those events actually are (resolved later the same evening)

- A deliberate sink-overflow test at 00:41 UTC produced **exactly the same signature**:
  3.71 → 4.26 (WARN) → 4.82 (WARN2) → 5.21 (ALARM) → peak 5.40 in, cleared in ~10 s, all
  with `k=21 ow=0 ne=0`. All three tiers fired in order, as designed.
- **So the three "events" above are routine kitchen water use, not incidents.** Running a
  sink or filling a pot pushes the drain past 5" for a few seconds. The drain handled
  every one of them.
- **This inverts the alerting rule.** An earlier note in this entry said to alert on
  `max()` over the window; that is wrong and would page on every pot of water. The
  discriminator is not magnitude but **persistence** — a discharge clears in ~10 s, a real
  backup stays up for minutes. Alert on a **sustained** condition (level above threshold
  continuously for 2–5 min).
- **The on-device buzzer has the same flaw.** The alarm tier is evaluated instantaneously
  with no dwell, so normal kitchen use sounds 95 dB. That is almost certainly why the
  alarm was found acknowledged on arrival, and a box that cries wolf is a box that gets
  unplugged — which may well explain the 25 days of missing power. **Highest-value v8
  change: a 30–60 s dwell above ALARM before the buzzer sounds, LED bar left
  instantaneous.**

### Other findings

- **The unit was unpowered for roughly 25 of the last 56 days** — session gaps
  09-13→09-27 and 09-30→10-11 with no records at all. This, not WiFi, is now the main
  threat to remote monitoring. The RE315 is on a church outlet too and has the same
  exposure. Sessions 2 and 3 carry absolute timestamps that cannot be reconciled with a
  cold start; treat their absolute dates as unverified (session 4 is solidly anchored and
  holds all the events above).
- Laptop on `SJC Parish Guest` resolved `drain.local` on the extender's side, so the
  guest network has **no client isolation**.
- **Synthetic data warning:** the float was moved by hand on the bench from about
  2026-10-23 00:16 UTC while the unit was online and delivering. Those points are in
  InfluxDB as `drain-1` and cannot be deleted with the current append-only token. Ignore
  that window.
- `ReadExisting()` in a serial poll loop produced 8,500 duplicate fragments around 59 real
  lines. Read raw bytes gated on `BytesToRead` instead.

- **Still open:** measure RSSI at the mount point; confirm it reads 25–26 cm empty after
  remounting with the disturbed float/liner; find an outlet that does not get unplugged;
  **build the Grafana alert rule and notification channel** — there is remote data but
  still no remote alerting. Lower priority: `DIST_MIN_CM` was lowered 10→7 and a 5.5"
  overflow is raw ≈13.6 cm, so the floor could go back to 10 with margin and gate out
  near-field artifacts.

## 2026-10-10/11 — weekend hotspot drive-by (reported 10-13)

- A new block arrived in InfluxDB: Thu 09-03 07:56 through Mon 09-07 16:50 EDT, 630
  readings at the calm 10-min cadence, all dry, delivered via a new hotspot IP
  (10.205.127.210). Nothing newer than 09-07 has arrived, and nothing from 09-01 12:12 to
  09-03 07:56.
- Every delivered row is stamped fw=5: **the unit did not update to v7.** The OTA needs
  about two minutes on the hotspot after association; a visit shorter than that aborts
  the download and the check does not retry for 6 h.
- No live readings from the visit (rssi=0 on every row), so it was shorter than one
  10-min calm heartbeat.
- The block boundaries (09-01 12:12, 09-03 07:56, 09-07 16:50) all look like power
  interruptions: each new block starts and ends at an odd time with rst=1 (power-on).
  Suspect the outlet is being used or unplugged by kitchen staff.
- Why exactly this block became deliverable on this visit and not on 09-13 is not fully
  explained from the data; `/log.csv` and the serial `LOG: mounted — session=N` line
  on the next visit will settle it.
- **Next visit:** stay on the hotspot at least 10 min and watch Grafana for fw to flip to
  7. If it does not within 3 min, power-cycle the unit (the OTA check fires 60 s after
  boot). Pull `/log.csv` before that. Look at where the unit is plugged in.

## 2026-10-13 — WiFi bridge bench-tested, v7 built

- Bridge plan: a TP-Link RE315 dual-band extender joins the church's 5 GHz-only guest
  network and broadcasts a 2.4 GHz network named `DrainMonitor` for the unit. (An RE105
  bought 09-29 was 2.4 GHz only and could not do this; returned.)
- Bench test at home: RE315 extending the 5 GHz band only, `DrainMonitor` on 2.4 GHz.
  Bench ESP32 running experiments/BenchTest joined it (IP 192.168.68.201, RSSI -45) and
  wrote to InfluxDB, HTTP 204. End-to-end TLS through the bridge works.
- BenchTest now tags its test point `bench` instead of `drain-1` so it stays off the
  dashboard series.
- `DrainMonitor` added first in both secrets files; hotspot remains the fallback.
- **v7 released** (same code as v6 plus the new credentials) and the manifest flipped
  to 7. The unit goes v5 to v7 directly on the next visit; v6 is skipped.
- **Still to verify on site:** captive portal or client isolation on the guest network,
  NTP (UDP 123) passing through, and signal at the kitchen. Test with a phone on
  `DrainMonitor` before mounting the extender.

## 2026-09-13 — firmware v6 released (flush-before-NTP fix)

- flushChunk() now stops at the first record from the current boot while NTP is still
  pending, so a reboot no longer discards its own backlog from the InfluxDB path.
- Sensing, thresholds and the on-flash record format are unchanged; the stored log
  survives the update.
- Release fw-v6 published and the manifest flipped. Lands on the next visit: stay on the
  hotspot 10+ min, expect the OTA reboot within a minute or two, confirm fw=6 in Grafana.
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
