# OTA update process (pull-based)

The deployed firmware ([../src/StJoesDrainMonitor.ino](../src/StJoesDrainMonitor.ino))
updates itself. There is **no push from the IDE** — the device drives everything:

1. Every ~6 h (and ~1 min after boot), if the water level is low and WiFi is up, the
   device fetches [`manifest.txt`](manifest.txt) over HTTPS.
2. It compares the manifest's version to its compiled-in `FW_VERSION`.
3. If the manifest version is **greater**, it downloads the `.bin` at the URL in the
   manifest and flashes it, then reboots into the new image.
4. Equal or lower → no-op. (This is what prevents a reflash loop.)

`manifest.txt` is served straight off the `main` branch via
`raw.githubusercontent.com`, so editing it here **is** publishing.

## Manifest format

Exactly two lines, no JSON:

```
<version integer>
<https URL of the .bin>
```

## Publishing a new firmware version

Do these in order — **bin first, manifest last** — so a device can never fetch a
manifest that points at a `.bin` that isn't uploaded yet.

1. **Bump the version in the source.** Edit `const int FW_VERSION` in
   `../src/StJoesDrainMonitor.ino` to the new number (e.g. `2`).
2. **Build** the release binary:
   ```sh
   cd ..            # into firmware/
   pio run          # output: .pio/build/esp32dev/firmware.bin
   ```
3. **Cut a GitHub release** and attach the binary. The asset's download filename is the
   file's basename and the manifest URL must match it, so **rename the bin before upload**
   — the `path#label` form only sets a display *label*, NOT the download filename:
   ```sh
   cp .pio/build/esp32dev/firmware.bin StJoesDrainMonitor-v2.bin
   gh release create fw-v2 \
     --title "Firmware v2" --notes "what changed" \
     StJoesDrainMonitor-v2.bin
   ```
4. **Flip the manifest** — edit `manifest.txt` to:
   ```
   2
   https://github.com/SsimonSA/church-drain-monitor/releases/download/fw-v2/StJoesDrainMonitor-v2.bin
   ```
   then commit + push to `main`.

Within one check interval each device sees version 2 > its running 1, updates, and
reboots. Confirm success in Grafana: the `fw` field in the `depth` measurement flips
to `2`, and the unit keeps reporting. **To roll back**, point the manifest back at the
older release and its lower version number — but note a device already running the
newer build won't downgrade (its `FW_VERSION` is now higher), so a true rollback
usually means cutting a new release with a *higher* number containing the old code.

## Notes / limitations

- **The reading log lives in the `spiffs` partition, and OTA never touches it.** Only
  the app partition is rewritten, so a firmware update keeps the stored history. Two
  things do wipe it: a change to the partition table (don't), and a change to the
  on-flash record layout. The latter is guarded — `struct Reading` has a
  `static_assert` on its size and the log carries a `LOG_MAGIC` stamp, so a mismatched
  build wipes the log deliberately at boot instead of decoding old bytes as garbage
  depths. **If you change `struct Reading`, bump `LOG_MAGIC`.**
- **First boot of v5 formats that partition** (it was never used before), which adds a
  few seconds to that one boot.

- **Download integrity, not boot-rollback.** The ESP32 verifies the image (size +
  checksum) before switching the boot partition, so a corrupt/truncated *download*
  can't brick the device — the current firmware keeps running. It does **not** auto-
  revert a firmware that flashes cleanly but then crashes at runtime; the Arduino-ESP32
  bootloader isn't built with app-rollback enabled. Test each build on the bench before
  cutting a release.
- **Local post-update self-check (network-independent).** The firmware classifies each
  boot from `esp_reset_reason()` and an NVS crash counter, and logs it over serial:
  `first boot of fw vN`, `clean boot OK`, `CRASH (PANIC/WDT) bad-boot k/3 X`, or a loud
  `SUSPECT BUILD` after 3 firmware crashes. Because it's local, it flags a bad build
  even at a site with no WiFi — you don't have to infer health from dashboard silence.
  **Brownouts are called out separately** as a power fault and never count toward the
  suspect threshold (a flaky supply must not masquerade as a bad binary). The last reset
  reason is also sent as the `rst` telemetry field so Grafana can annotate reboots.
  This surfaces a bad build; it still can't *auto-revert* one (that needs the rollback
  bootloader) — a confirmed-bad unit needs a USB reflash.
- **Safe mode.** Hitting the same 3-crash threshold also disables the subsystems that
  aren't needed to warn anybody: the filesystem and the web server. The sensor, LED
  bar, overflow lamp and buzzer keep running, and telemetry falls back to the RAM
  buffer. It is not a rollback — the unit still needs a USB reflash — but a bad build
  degrades into a working drain alarm instead of a dead box, which matters because
  recovery requires physically visiting the site.
- **TLS is encrypt-only** (`setInsecure()`), matching the telemetry path. Fine for a
  public binary; if you later want MITM protection, pin GitHub's root CA or add a
  SHA-256 check to the manifest.
- **Power first.** Do not rely on OTA until the field unit has the ≥2 A supply + bulk
  cap — a brownout during the flash write is the one thing that *can* brick it.
