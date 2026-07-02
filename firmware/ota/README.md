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
3. **Cut a GitHub release** and attach the binary, renamed to match the version:
   ```sh
   gh release create fw-v2 \
     --title "Firmware v2" --notes "what changed" \
     ".pio/build/esp32dev/firmware.bin#StJoesDrainMonitor-v2.bin"
   ```
   (The `#name` suffix sets the asset's download filename.)
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

- **Download integrity, not boot-rollback.** The ESP32 verifies the image (size +
  checksum) before switching the boot partition, so a corrupt/truncated *download*
  can't brick the device — the current firmware keeps running. It does **not** auto-
  revert a firmware that flashes cleanly but then crashes at runtime; the Arduino-ESP32
  bootloader isn't built with app-rollback enabled. Your safety net there is the
  Grafana `fw`/liveness signal — if a unit goes silent after an update, it needs a USB
  reflash. Test each build on the bench before cutting a release.
- **TLS is encrypt-only** (`setInsecure()`), matching the telemetry path. Fine for a
  public binary; if you later want MITM protection, pin GitHub's root CA or add a
  SHA-256 check to the manifest.
- **Power first.** Do not rely on OTA until the field unit has the ≥2 A supply + bulk
  cap — a brownout during the flash write is the one thing that *can* brick it.
