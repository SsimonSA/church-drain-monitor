/*
 * StJoesDrainMonitor  —  ESP32 + HC-SR04 float-in-pipe water-level monitor
 * ---------------------------------------------------------------------------
 * - SR04 float-in-pipe sensor + two-point calibration -> water depth in inches.
 * - LOCAL indicators (work even if WiFi/cloud is down): 5-LED level bar, a
 *   blinking overflow lamp, and a 95 dB piezo buzzer with TWO audible tiers:
 *   WARN (>=4.0") = short chirp every few seconds (lamp slow-blinks) to prompt the
 *   dishwasher to ease off; ALARM (>=5.0", below the ~5.5" physical overflow) =
 *   full beep-until-acknowledged (lamp fast-blinks). All levels field-tunable.
 * - Momentary pushbutton: SHORT press = acknowledge/silence the current alarm
 *   (lamp keeps blinking); LONG press (~2 s) = master mute toggle (buzzer on/off,
 *   persists across events, re-arms on reboot).
 * - Cloud telemetry: posts depth to InfluxDB (via Caddy/HTTPS) for the Grafana
 *   dashboard, with adaptive cadence + offline store-and-forward.
 *
 * STORE AND FORWARD
 *   Readings are stamped with UPTIME, not wall-clock, so logging never depends on
 *   having had a network. When NTP eventually lands we learn the epoch at uptime 0
 *   and back-date the whole backlog on its way out. A device that powers up out of
 *   range still records everything; it just delivers late. The backlog leaves in
 *   bounded chunks (never one giant POST) and drains flat out only while the water
 *   is low, so a backfill can't stall the alarm.
 *
 *   The queue lives in FLASH (LittleFS, two rotating files ~45k readings total), so
 *   it survives reboots, brownouts and power cuts — RAM holds only a small staging
 *   batch. If the filesystem can't be mounted the firmware falls back to the old
 *   RAM-only ring buffer rather than losing telemetry outright.
 *
 * LOCAL HISTORY DOWNLOAD
 *   The device also serves its own log over plain HTTP on the LAN:
 *     http://drain.local/log.csv   (or http://<ip>/log.csv — the IP is in telemetry)
 *   Park next to the box, turn on a hotspot, open the URL. This path needs no
 *   InfluxDB, no token, no retention window and no internet at all.
 *
 * The LED bar / lamp / buzzer are driven from the ESP32's OWN reading every loop,
 * independent of the network — the box is a self-contained alarm first, a cloud
 * sensor second.
 *
 * TELEMETRY CADENCE
 *   - ACTIVE (level >= 0.5 in): send every second.
 *   - After dropping below 0.5 in, keep sending every second for a 5-min SETTLE
 *     window (recession tail), then CALM = 10-min heartbeat (also a liveness ping).
 *   - Any rise back to >= 0.5 in resumes per-second sending + restarts the timer.
 *
 * ===== GPIO MAP (ESP32-WROOM-32) =====
 *   SR04:    TRIG=22  ECHO=21 (reused old VL53 I2C pins; ECHO via 1k/2k divider; SR04 powered at 5V)
 *   LED bar: LG=18 HG=5 LY=17 HY=16 LR=4   (active-HIGH, climbing with level)
 *   Lamp:    overflow=2   (red, blinks at overflow)
 *   Buzzer:  27  (active piezo via logic-level N-MOSFET low-side switch; on the 12V rail)
 *   Button:  32  (momentary to GND, uses internal pull-up)
 *   Btn LED: 25  (12V ring LED via N-MOSFET low-side switch, active-HIGH:
 *                 solid=armed, off=muted, fast-blink=unacknowledged alarm)
 *   Power:   USB 5V powers the ESP32; ESP32 5V pin -> MT3608 boost (set 12V) -> 12V rail -> buzzer/LED
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>   // pull-based OTA (device fetches a manifest, self-flashes from a GitHub release)
#include <WebServer.h>    // on-device /log.csv download (grab history without the cloud path)
#include <ESPmDNS.h>      // advertise drain.local so you needn't hunt for a DHCP address
#include <LittleFS.h>     // persistent reading log — survives reboot, brownout, power loss
#include <Preferences.h>  // NVS-backed boot/version state for the post-update self-check
#include <esp_system.h>   // esp_reset_reason()
#include <time.h>

#include "secrets.h"   // WIFI_NETS[], N_WIFI, INFLUX_TOKEN — gitignored; see secrets.example.h

// ====== WIFI (networks + priority order live in secrets.h; falls through on failure, then cycles) ======
const unsigned long WIFI_ATTEMPT_MS = 10000UL; // per-network connect timeout

// ====== NTP ======
// The clock no longer gates LOGGING (readings carry uptime and are back-dated at flush
// time — see g_bootEpoch), but it still gates DELIVERY, since InfluxDB needs absolute
// timestamps. The boot-time sync attempt fails whenever the device powers up out of WiFi
// range, and ESP-IDF's background SNTP client only retries about hourly. serviceNtp()
// re-arms on every down->up WiFi transition so a device that has been offline can ship
// its backlog within seconds of a network appearing.
const time_t        NTP_VALID_EPOCH = 1700000000; // any time_t above this is a real clock
const unsigned long NTP_REARM_MS    = 60000UL;    // while online but unsynced, retry this often

// ====== INFLUXDB (via Caddy HTTPS reverse proxy) ======
const char* INFLUX_WRITE_URL =
    "https://stjoesdrain.eastus.cloudapp.azure.com/api/v2/write?org=stjoes&bucket=drain&precision=s";
// INFLUX_TOKEN is provided by secrets.h (gitignored)
const char* DEVICE_TAG = "drain-1";
// TLS: encrypt but skip cert validation (setInsecure); the write token is the gate.

// ====== OTA (pull-based) ======
// The device periodically fetches a plain-text manifest and, if it advertises a
// version newer than FW_VERSION, downloads that .bin and self-flashes. There is NO
// push path — updates are driven entirely by the device. Publishing an update =
// upload the new .bin as a GitHub release asset, then bump the manifest (see
// firmware/ota/README.md). Never flashes while the water is high (see loop()).
//
// >>> BUMP FW_VERSION ON EVERY RELEASE. The manifest's version must EXCEED this to
//     trigger an update; equal or lower is a no-op, which is what stops reflash loops.
const int FW_VERSION = 5;
const char* OTA_MANIFEST_URL =
    "https://raw.githubusercontent.com/SsimonSA/church-drain-monitor/main/firmware/ota/manifest.txt";
const unsigned long OTA_CHECK_INTERVAL_MS = 6UL * 3600UL * 1000UL;  // re-check every 6 h
const unsigned long OTA_FIRST_CHECK_MS    = 60000UL;               // first check ~1 min after boot
                                                                   // (long enough to send startup telemetry first)

// Post-update self-check: prove a new binary is healthy LOCALLY (no network needed).
const unsigned long VALIDATE_STABLE_MS = 60000UL;  // run this long without a crash -> mark the firmware good
const int OTA_BAD_BOOT_LIMIT = 3;                  // this many firmware-CRASH reboots since an update -> loud SUSPECT warning
                                                   // AND safe mode (see g_safeMode)

// ====== SENSOR (HC-SR04) ======
const int   TRIG_PIN = 22;   // reused from old VL53 I2C (SCL); direct 3.3V drive OK
const int   ECHO_PIN = 21;   // reused from old VL53 I2C (SDA); SR04 5V echo -> via 1k/2k divider
const int   SAMPLES          = 21;       // pings per reading (odd -> clean median)
const unsigned long ECHO_TIMEOUT_US = 30000UL;
const int   PING_GAP_MS      = 8;        // between pings (short; we service alarm/button in the gap)
const float US_PER_CM = 58.0f;

// ====== CALIBRATION — direct linear fit: level(in) = CAL_SLOPE*raw_cm + CAL_INTERCEPT ======
// Least-squares fit from real-water test 2026-07-01 (ultrasonic-tank, lined 2" PVC, new float):
//   raw 23.6cm=1"  21.4=2"  19.0=3"  17.5=4"  14.45=5"   (1/2/3/5 collinear; 4" slightly noisy)
// Below DEADBAND_IN the float rests on the base (nonlinear/fuzzy) -> report exactly 0.
// To recalibrate: refit these two numbers from fresh (raw_cm, inches) pairs.
const float CAL_SLOPE     = -0.4468f;   // inches per cm (raw shrinks as water rises)
const float CAL_INTERCEPT = 11.57f;     // inches at raw_cm = 0
const float DEADBAND_IN   = 1.4f;       // empty -> clamp to 0.00. Generous: float rest wanders up to ~1.0",
                                        // so 1.4 keeps the 2nd green dark at empty (HG then lights at ~1.4" real).

// ====== ROBUSTNESS (validated on the bench) ======
// #1 PLAUSIBILITY GATE — the float echo must land in this one-way distance band;
//    anything nearer (wall/rim/cross-talk) or farther (no float) is dropped BEFORE
//    the median, so a spurious short echo can't pull the reading down. Generous for
//    now (tube is 30.7 cm); tighten to the real travel band once field-verified.
const float DIST_MIN_CM = 7.0f;    // lowered from 10: at ~5.5" overflow raw drops to ~9.9 cm; 10 would clip it
const float DIST_MAX_CM = 30.0f;
// #3 SLEW LIMIT — real water can't move this many inches between 1 s samples, so a
//    bigger jump is clamped. One stray sample can't spike the level or trip an alarm.
const float MAX_SLEW_IN = 1.5f;

// ====== LOCAL INDICATORS ======
#define PIN_LED_LG 18   // green  (lowest bar segment)
#define PIN_LED_HG 5    // green
#define PIN_LED_LY 17   // yellow
#define PIN_LED_HY 16   // yellow
#define PIN_LED_LR 4    // red    (highest bar segment)
#define PIN_LAMP   2    // overflow lamp (blinks)
#define PIN_BUZZER 27   // active piezo (via logic-level N-MOSFET from the 12V rail)
#define PIN_BUTTON 32   // momentary, to GND, INPUT_PULLUP
#define PIN_BTN_LED 25  // 12V ring LED (status) via N-MOSFET from 12V rail, active-HIGH

// Bar climbs with depth; lowest green is an always-on "alive/powered" indicator.
const float BAR_HG_IN = 1.0f;
const float BAR_LY_IN = 2.0f;
const float BAR_HY_IN = 3.0f;
const float BAR_LR_IN = 4.0f;
// Two audible tiers so the dishwasher gets time to shut the faucet before overflow.
// (Physical overflow is ~5.5"; the urgent alarm trips BELOW that for reaction time.)
// All field-tunable.
const float WARN_IN        = 4.0f;   // pre-overflow tier 1: slow chirp
const float WARN2_IN       = 4.5f;   // pre-overflow tier 2: faster chirp (escalating)
const float ALARM_IN       = 5.0f;   // urgent: full beep-until-acknowledged
const float ALARM_CLEAR_IN = 4.8f;   // hysteresis: re-arm the ack below this

// Overflow lamp runs on the ESP32 LEDC (hardware PWM), so its blink is timer-generated and
// stays perfectly even even while the loop is busy (network upload / ping burst block it).
#define LAMP_LEDC_CH   4                     // an otherwise-unused LEDC channel
const int      LAMP_LEDC_BITS = 20;          // 20-bit resolution -> supports ~0.07..76 Hz
const uint32_t LAMP_LEDC_DUTY = 1UL << 19;   // 50% duty = symmetric square-wave blink
const float LAMP_HZ_ALARM = 5.0f;            // ALARM: 5 Hz strobe
const float LAMP_HZ_WARN2 = 1.67f;           // WARN2: faster blink (~300 ms period)
const float LAMP_HZ_WARN  = 0.71f;           // WARN:  slow blink (~700 ms period)
const unsigned long BTN_LED_BLINK_MS = 150UL;// button lamp fast-blink = unacked alarm
const unsigned long BEEP_MS       = 350UL;   // ALARM buzzer beep on/off period
const unsigned long WARN_CHIRP_GAP_MS  = 3500UL; // silence between WARN (tier 1) chirps
const unsigned long WARN2_CHIRP_GAP_MS = 1200UL; // silence between WARN2 (tier 2) chirps — faster
const unsigned long WARN_CHIRP_LEN_MS  = 60UL;   // length of each warn chirp (both tiers)
const unsigned long LONG_PRESS_MS = 2000UL;  // hold this long = master mute toggle
const unsigned long DEBOUNCE_MS   = 30UL;

// ====== TELEMETRY POLICY ======
const unsigned long SAMPLE_INTERVAL_MS = 1000UL;   // read + active-send cadence
const unsigned long SETTLE_MS          = 300000UL; // 5 min fast after dropping below threshold
const unsigned long SLOW_INTERVAL_MS   = 600000UL; // 10 min heartbeat once calm
const unsigned long WATCHDOG_MS        = 1800000UL;
const float TELEMETRY_THRESHOLD = 0.5f;            // active/calm boundary for sending

// ====== buffered readings ======
// A reading is timestamped one of two ways, distinguished by RD_RESOLVED:
//   resolved   -> `t` is a real epoch, settled for good
//   unresolved -> `t` is seconds since the boot of session `sess`
// Uptime alone was enough while the queue lived in RAM, but the flash log now OUTLIVES
// reboots, and uptime restarts at zero on each one. Anchoring an old record to the current
// boot's epoch would scatter history across the wrong days — worse than not sending it —
// so every record carries the session it was measured in and is only ever dated against
// that session's own anchor. See resolveEpoch().
const uint16_t RD_RESOLVED = 0x0001;
struct Reading {
  uint32_t t;                                        // epoch if resolved, else uptime seconds
  float in; float rawCm;
  int16_t rssi; int16_t kept; int16_t outWin; int16_t noEcho;
  uint16_t sess;                                     // boot session that measured this
  uint16_t flags;
};
// The on-flash log stores these verbatim, so the size is part of the file format: a change
// here silently misreads every older record. Bump LOG_MAGIC below if this struct changes.
static_assert(sizeof(Reading) == 24, "Reading size is the on-flash record size — bump LOG_MAGIC if you change it");

// RAM staging. With the filesystem up this is just a write-behind batch on its way to
// flash; if the mount failed it degrades into the old standalone ring buffer.
const int BUF_MAX = 2000;
Reading rbuf[BUF_MAX];
int rcount = 0;

// Backlog leaves in bounded batches: ~200 points is ~30 KB of line protocol, which the
// heap can hold comfortably. One giant POST of a full buffer would both blow memory and
// freeze the alarm for the length of the upload.
const int FLUSH_CHUNK = 200;

// ====== persistent log (LittleFS) ======
// Two files used as a coarse ring: we always append to the ACTIVE one and, when it fills,
// delete the other (the older generation) and start writing there. Coarse rotation rather
// than a true record-level ring because deleting from the front of a file isn't a thing —
// the cost is that the backlog drops in ~448 KB steps rather than one reading at a time.
//
// Sizing: the `spiffs` partition in default.csv is ~1.4 MB; 2 x 448 KB leaves LittleFS
// plenty of free blocks (it degrades badly near full) and holds ~45,000 readings — about
// 12 h of continuous 1 Hz logging during a backup, or ~10 months of calm heartbeats.
const char*  LOG_PATH[2]    = { "/log0.bin", "/log1.bin" };
const size_t LOG_FILE_MAX   = 448UL * 1024UL;
const size_t LOG_RECSZ      = sizeof(Reading);
const uint32_t LOG_MAGIC    = 0x44524E32UL;   // 'DRN2' — record-format version guard
// Write-behind policy: batch records so a flash page isn't burned per reading, but never
// hold data in volatile RAM for long — the whole point is surviving an unannounced power cut.
const int           LOG_COMMIT_N  = 32;
const unsigned long LOG_COMMIT_MS = 60000UL;

bool     g_fsOk    = false;   // false -> RAM-only fallback (see rbuf)
uint8_t  g_logAct  = 0;       // file we append to
uint8_t  g_logRd   = 0;       // file we are delivering from
uint32_t g_logOff  = 0;       // byte offset of the next undelivered record in g_logRd
uint16_t g_sess    = 0;       // this boot's session id (NVS counter, increments every boot)
// The most recent PREVIOUS session that managed to learn an epoch anchor. Records committed
// early in that session — before its own NTP sync landed — are still sitting unresolved in
// the log, and this is what lets a later boot date them instead of discarding them.
uint16_t g_anchSess  = 0;
uint32_t g_anchEpoch = 0;
Preferences logPrefs;
unsigned long lastCommitMs = 0;
// Undelivered-record count, refreshed once per sample tick and decremented as chunks land.
// Cached because the true count needs a filesystem stat, and the burst-drain test below
// runs every loop iteration (~200 Hz) — statting flash that often would be absurd.
uint32_t g_pending = 0;

// ====== diagnostics from the most recent reading (raw distance + ping quality) ======
float g_rawCm  = NAN;   // median of the in-window pings, in cm (pre-calibration distance)
int   g_kept   = 0;     // pings that landed inside the plausibility window
int   g_outWin = 0;     // pings with an echo but outside the window (gate-rejected)
int   g_noEcho = 0;     // pings with no echo at all

// ====== uptime / absolute-time anchor ======
// Epoch corresponding to uptime 0, or 0 while still unknown. Learned once, the first time
// NTP produces a real clock: bootEpoch = now - uptime. Every buffered reading then becomes
// absolute (bootEpoch + up) — including the ones recorded hours earlier with no clock at
// all. This is the whole trick behind offline logging.
//
// It survives a soft reboot for free: ESP.restart() and the OTA reboot keep the RTC domain
// powered, so time() is already valid in setup() and the anchor is re-established on the
// first serviceNtp() call. Only a true power cycle loses it — and readings taken during a
// powered-down-then-offline stretch genuinely have no recoverable absolute time. A
// battery-backed RTC (DS3231) would close that last gap if the site ever needs it.
uint32_t g_bootEpoch = 0;

// millis() wraps every ~49.7 days; this accumulates across the wrap so a long offline
// stretch can't fold the backlog's timestamps back on themselves. Safe because it is
// called at least once per sample tick, far more often than the wrap period.
uint32_t uptimeSec() {
  static uint32_t lastMs = 0;
  static uint32_t wraps  = 0;
  uint32_t ms = millis();
  if (ms < lastMs) wraps++;
  lastMs = ms;
  return (uint32_t)(((uint64_t)wraps * 4294967296ULL + ms) / 1000ULL);
}

// ====== state ======
unsigned long lastSampleMs = 0;
unsigned long lastEnqueueMs = 0;
unsigned long lastSendOkMs = 0;
unsigned long lastOtaCheckMs = 0;

// ---- post-update self-check state ----
Preferences otaPrefs;
esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;
bool     g_pendingValidation = false;   // running an as-yet-unproven firmware
uint32_t g_bootCount = 0;
uint32_t g_badBoots  = 0;               // consecutive firmware-crash reboots since last good boot
// After OTA_BAD_BOOT_LIMIT firmware crashes we stop starting the subsystems that are not
// needed to warn somebody about water on the floor — the filesystem and the web server.
// The box has no auto-rollback (the Arduino bootloader isn't built with it) and recovery
// means a USB reflash on site, so the failure mode that actually matters is "the alarm
// stopped working". Whatever is crashing, the LED bar, lamp and buzzer still run: they
// depend only on the sensor and are driven before any of this is touched.
bool     g_safeMode  = false;
unsigned long g_bootMs = 0;
bool isBelow = false;
unsigned long belowSinceMs = 0;

float lastLevelIn = 0.0f;     // most recent valid depth (drives indicators)
bool  acked = false;          // current alarm acknowledged (short press)
bool  muted = false;          // master mute (long press), persists until toggled
bool  beepOn = false;
unsigned long lastBeepToggle = 0;
bool  lampOn = false;
unsigned long lastLampBlink = 0;
bool  btnLedOn = false;
unsigned long lastBtnLedBlink = 0;

// ---------- sensor ----------
float pingOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1.0f;
  return us / US_PER_CM;
}

void sortAsc(float *a, int n) {
  for (int i = 1; i < n; i++) {
    float key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
}

void serviceFast();   // fwd decl (called between pings to keep alarm/button responsive)

// Returns calibrated water level in inches, or NAN on no echo.
float readLevelInches() {
  static float buf[SAMPLES];
  int valid = 0, outWin = 0, noEcho = 0;
  for (int i = 0; i < SAMPLES; i++) {
    float d = pingOnce();
    if (d <= 0)                                  noEcho++;          // no echo at all
    else if (d < DIST_MIN_CM || d > DIST_MAX_CM) outWin++;          // #1 gate: rejected (out of window)
    else                                         buf[valid++] = d;  // in-window keeper
    serviceFast();           // keep buzzer/lamp/button/LEDs live during the burst
    delay(PING_GAP_MS);
  }
  g_kept = valid; g_outWin = outWin; g_noEcho = noEcho;
  if (valid == 0) { g_rawCm = NAN; return NAN; }
  sortAsc(buf, valid);
  float median = buf[valid / 2];
  g_rawCm = median;          // raw distance (cm) behind this reading, for diagnostics
  float levelIn = CAL_SLOPE * median + CAL_INTERCEPT;   // direct linear calibration
  if (levelIn < DEADBAND_IN) levelIn = 0.0f;            // fuzzy resting zone -> report empty
  return levelIn;
}

// ---------- local indicators ----------
void updateBarGraph() {
  float L = lastLevelIn;
  digitalWrite(PIN_LED_LG, HIGH);              // always on while running = "alive"
  digitalWrite(PIN_LED_HG, L >= BAR_HG_IN);
  digitalWrite(PIN_LED_LY, L >= BAR_LY_IN);
  digitalWrite(PIN_LED_HY, L >= BAR_HY_IN);
  digitalWrite(PIN_LED_LR, L >= BAR_LR_IN);
}

// Only reconfigures the hardware PWM when the tier CHANGES; in between, the LEDC peripheral
// blinks the lamp on its own — immune to loop stalls (network/ping), so the strobe is dead even.
void handleOverflowLamp() {
  static int lampMode = -1;                  // -1 uninit / 0 off / 1 warn / 2 warn2 / 3 alarm
  float L = lastLevelIn;
  int m = (L >= ALARM_IN) ? 3 : (L >= WARN2_IN) ? 2 : (L >= WARN_IN) ? 1 : 0;
  if (m == lampMode) return;                 // unchanged -> leave the hardware blinking
  lampMode = m;
  float hz = (m == 3) ? LAMP_HZ_ALARM : (m == 2) ? LAMP_HZ_WARN2 : (m == 1) ? LAMP_HZ_WARN : 0.0f;
  if (hz <= 0.0f) { ledcWrite(LAMP_LEDC_CH, 0); return; }   // off (solid low)
  ledcSetup(LAMP_LEDC_CH, hz, LAMP_LEDC_BITS);
  ledcWrite(LAMP_LEDC_CH, LAMP_LEDC_DUTY);                  // hardware square-wave blink at hz
}

void chirp() {  // brief blip to confirm a long-press mute toggle
  digitalWrite(PIN_BUZZER, HIGH);
  delay(70);
  digitalWrite(PIN_BUZZER, LOW);
}

void serviceButton() {
  static bool stable = HIGH, lastRaw = HIGH, longFired = false;
  static unsigned long lastChange = 0, pressStart = 0;
  unsigned long ms = millis();
  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastRaw) { lastRaw = raw; lastChange = ms; }
  if (ms - lastChange > DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable == LOW) {            // press begins
      pressStart = ms; longFired = false;
    } else {                        // release
      if (!longFired) acked = true; // short press = acknowledge current alarm
    }
  }
  // long press fires while still held
  if (stable == LOW && !longFired && (ms - pressStart >= LONG_PRESS_MS)) {
    longFired = true;
    muted = !muted;
    chirp();
  }
}

// Four audible states: OFF / WARN (slow chirp) / WARN2 (fast chirp) / ALARM (beep-until-ack).
void serviceAlarm() {
  float L = lastLevelIn;
  unsigned long now = millis();
  if (L < ALARM_CLEAR_IN) acked = false;            // re-arm once it drops below alarm

  int mode = 0;                                     // 0=off 1=warn 2=warn-fast 3=alarm
  if (!muted) {
    if (L >= ALARM_IN && !acked)             mode = 3;
    else if (L >= WARN2_IN && L < ALARM_IN)  mode = 2;
    else if (L >= WARN_IN  && L < WARN2_IN)  mode = 1;
  }

  if (mode == 3) {                                  // ALARM: steady beep on/off
    if (now - lastBeepToggle >= BEEP_MS) { lastBeepToggle = now; beepOn = !beepOn; digitalWrite(PIN_BUZZER, beepOn); }
  } else if (mode == 1 || mode == 2) {              // WARN tiers: chirp; tier 2 chirps faster
    unsigned long gap = (mode == 2) ? WARN2_CHIRP_GAP_MS : WARN_CHIRP_GAP_MS;
    if (!beepOn && now - lastBeepToggle >= gap)                   { beepOn = true;  lastBeepToggle = now; digitalWrite(PIN_BUZZER, HIGH); }
    else if (beepOn && now - lastBeepToggle >= WARN_CHIRP_LEN_MS) { beepOn = false; lastBeepToggle = now; digitalWrite(PIN_BUZZER, LOW); }
  } else {                                          // OFF
    if (beepOn) { beepOn = false; digitalWrite(PIN_BUZZER, LOW); }
    lastBeepToggle = now;
  }
}

// Illuminated-button lamp: solid = armed/normal, off = master-muted,
// fast-blink = active alarm not yet acknowledged.
void serviceButtonLed() {
  if (muted) {
    digitalWrite(PIN_BTN_LED, LOW);
    btnLedOn = false;
  } else if (lastLevelIn >= ALARM_IN && !acked) {
    if (millis() - lastBtnLedBlink >= BTN_LED_BLINK_MS) {
      lastBtnLedBlink = millis();
      btnLedOn = !btnLedOn;
      digitalWrite(PIN_BTN_LED, btnLedOn);
    }
  } else {
    digitalWrite(PIN_BTN_LED, HIGH);
    btnLedOn = true;
  }
}

// Runs every loop iteration AND between pings -> responsive alarm/button.
void serviceFast() {
  serviceButton();
  updateBarGraph();
  handleOverflowLamp();
  serviceAlarm();
  serviceButtonLed();
}

// ---------- persistent log ----------
// The absolute time of a reading, or 0 if it can never be known. A record is datable if it
// was already resolved when written, or if it belongs to a session whose anchor we hold —
// this boot's, or the last previous boot that got one. Anything older stays undated: it is
// kept in the log (the shape of the history is still readable over /log.csv via uptime)
// but is never sent to InfluxDB, because a guessed timestamp would corrupt the dashboard.
uint32_t resolveEpoch(const Reading& r) {
  if (r.flags & RD_RESOLVED)                     return r.t;
  if (r.sess == g_sess     && g_bootEpoch)       return g_bootEpoch + r.t;
  if (r.sess == g_anchSess && g_anchEpoch)       return g_anchEpoch + r.t;
  return 0;
}

// Delivery cursor (which file, how far in) lives in NVS so a reboot mid-backlog resumes
// where it left off instead of replaying or dropping. Losing this is not fatal — Influx
// overwrites on identical measurement+tags+timestamp, so a replayed chunk is a no-op.
void logSaveState() {
  logPrefs.putUChar("act", g_logAct);
  logPrefs.putUChar("rd",  g_logRd);
  logPrefs.putUInt ("off", g_logOff);
}

size_t logFileSize(int i) {
  File f = LittleFS.open(LOG_PATH[i], "r");
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

void logInit() {
  g_fsOk = LittleFS.begin(true);          // true = format if the partition is blank/corrupt
  if (!g_fsOk) {
    Serial.println(F("LOG: LittleFS mount FAILED — falling back to RAM-only buffering"));
    return;
  }
  logPrefs.begin("log", false);

  // New boot = new session. Every reading taken from here on is stamped with it, which is
  // what keeps this boot's uptimes from being applied to an earlier boot's records.
  g_sess = (uint16_t)(logPrefs.getUShort("sess", 0) + 1);
  logPrefs.putUShort("sess", g_sess);
  g_anchSess  = logPrefs.getUShort("asess",  0);
  g_anchEpoch = logPrefs.getUInt  ("aepoch", 0);

  // A record-format change invalidates every stored byte; start clean rather than emit
  // garbage depth values into Grafana.
  if (logPrefs.getUInt("magic", 0) != LOG_MAGIC) {
    Serial.println(F("LOG: record format changed — wiping old log"));
    LittleFS.remove(LOG_PATH[0]);
    LittleFS.remove(LOG_PATH[1]);
    logPrefs.putUInt("magic", LOG_MAGIC);
    g_logAct = 0; g_logRd = 0; g_logOff = 0;
    logSaveState();
  } else {
    g_logAct = logPrefs.getUChar("act", 0);
    g_logRd  = logPrefs.getUChar("rd",  0);
    g_logOff = logPrefs.getUInt ("off", 0);
    if (g_logAct > 1) g_logAct = 0;
    if (g_logRd  > 1) g_logRd  = 0;
    // A truncated final record (power cut mid-write) would desync every later read, so
    // snap the cursor back to a record boundary and clamp it inside the file.
    g_logOff -= (g_logOff % LOG_RECSZ);
    if (g_logOff > logFileSize(g_logRd)) g_logOff = logFileSize(g_logRd);
  }
  Serial.printf("LOG: mounted — session=%u active=%d read=%d off=%lu  sizes %u/%u bytes"
                "  (prev anchored session %u @ %lu)\n",
                g_sess, g_logAct, g_logRd, (unsigned long)g_logOff,
                (unsigned)logFileSize(0), (unsigned)logFileSize(1),
                g_anchSess, (unsigned long)g_anchEpoch);
}

// Retire the oldest generation and start appending into its slot.
void logRotate() {
  int oldAct = g_logAct;
  int newAct = 1 - oldAct;
  Serial.printf("LOG: rotating — discarding %s (%u bytes, oldest history)\n",
                LOG_PATH[newAct], (unsigned)logFileSize(newAct));
  LittleFS.remove(LOG_PATH[newAct]);
  // If we were still delivering out of the file we just deleted, its undelivered tail went
  // with it; the only data left is the file we were appending to.
  if (g_logRd == newAct) { g_logRd = oldAct; g_logOff = 0; }
  g_logAct = newAct;
  logSaveState();
}

// Push the RAM staging batch into flash. Called before every read and on a size/time
// trigger, so at most LOG_COMMIT_MS of readings are ever exposed to a power cut.
void logCommit() {
  if (!g_fsOk || rcount == 0) return;

  // Settle timestamps BEFORE they hit flash wherever we can. A record written as a real
  // epoch needs no session context ever again, so it stays datable across any number of
  // future reboots — the session/anchor machinery is then only carrying the records taken
  // while this boot was still waiting on NTP.
  if (g_bootEpoch) {
    for (int i = 0; i < rcount; i++) {
      if (!(rbuf[i].flags & RD_RESOLVED)) {
        rbuf[i].t     = g_bootEpoch + rbuf[i].t;
        rbuf[i].flags |= RD_RESOLVED;
      }
    }
  }

  size_t need = (size_t)rcount * LOG_RECSZ;
  if (logFileSize(g_logAct) + need > LOG_FILE_MAX) logRotate();

  File f = LittleFS.open(LOG_PATH[g_logAct], "a");
  if (!f) { Serial.println(F("LOG: append open failed — holding batch in RAM")); return; }
  size_t wrote = f.write((const uint8_t*)rbuf, need);
  f.close();                              // close flushes; batch is durable past this point
  if (wrote != need) {
    // Partial write (filesystem full or failing). Keep the batch staged and let the next
    // commit retry after a rotation frees space.
    Serial.printf("LOG: short write %u/%u — keeping batch staged\n", (unsigned)wrote, (unsigned)need);
    return;
  }
  rcount = 0;
  lastCommitMs = millis();
}

// Records recorded but not yet delivered.
uint32_t logPendingRecords() {
  if (!g_fsOk) return rcount;
  uint32_t bytes = 0;
  if (g_logRd == g_logAct) {
    size_t s = logFileSize(g_logAct);
    bytes = (s > g_logOff) ? (s - g_logOff) : 0;
  } else {
    size_t sr = logFileSize(g_logRd);
    bytes = ((sr > g_logOff) ? (sr - g_logOff) : 0) + logFileSize(g_logAct);
  }
  return bytes / LOG_RECSZ + rcount;      // + whatever is still staged in RAM
}

// Copy up to `max` of the oldest undelivered records into `out`.
// Retires a spent older generation itself rather than relying solely on logAdvance(): a
// file whose byte length isn't an exact multiple of the record size (power cut mid-write)
// leaves the cursor short of EOF forever, which would wedge delivery permanently.
int logRead(Reading* out, int max) {
  if (!g_fsOk) return 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    int n = 0;
    File f = LittleFS.open(LOG_PATH[g_logRd], "r");
    if (f) {
      if (g_logOff < f.size() && f.seek(g_logOff))
        n = f.read((uint8_t*)out, (size_t)max * LOG_RECSZ) / LOG_RECSZ;
      f.close();
    }
    if (n > 0) return n;
    if (g_logRd == g_logAct) return 0;      // caught up with the live file — nothing more
    LittleFS.remove(LOG_PATH[g_logRd]);     // spent (or torn) older generation — reclaim it
    g_logRd  = g_logAct;
    g_logOff = 0;
    logSaveState();
  }
  return 0;
}

// Mark `n` records delivered, stepping to the next file once this one is exhausted.
void logAdvance(int n) {
  if (!g_fsOk) return;
  g_logOff += (uint32_t)n * LOG_RECSZ;
  if (g_logRd != g_logAct && g_logOff >= logFileSize(g_logRd)) {
    LittleFS.remove(LOG_PATH[g_logRd]);   // fully delivered — reclaim it
    g_logRd  = g_logAct;
    g_logOff = 0;
  }
  logSaveState();
}

// ---------- buffer / network ----------
// `up` is uptime seconds (see struct Reading). No clock is required to buffer — that is
// the point: a box that boots out of WiFi range still records its whole history.
void enqueue(uint32_t up, float in, float rawCm, int16_t rssi, int16_t kept, int16_t outWin, int16_t noEcho) {
  if (rcount >= BUF_MAX) {
    // With the filesystem up this only happens if flash writes are failing; without it,
    // this IS the buffer and dropping the oldest is the intended ring behaviour.
    memmove(rbuf, rbuf + 1, sizeof(Reading) * (BUF_MAX - 1));
    rcount = BUF_MAX - 1;
  }
  rbuf[rcount++] = { up, in, rawCm, rssi, kept, outWin, noEcho, g_sess, 0 };
  if (rcount >= LOG_COMMIT_N) logCommit();
}

// Time-based half of the write-behind policy: during calm 10-minute heartbeats a batch
// would otherwise sit in RAM for hours.
void serviceLogCommit() {
  if (rcount > 0 && millis() - lastCommitMs >= LOG_COMMIT_MS) logCommit();
}

// Ships up to FLUSH_CHUNK of the OLDEST buffered readings and drops them on success.
// Returns true only if something was actually delivered.
//
// Chunked rather than all-at-once for two reasons: a full buffer would be megabytes of
// line protocol (heap death), and a single upload that long would freeze the local alarm.
// Delivered points are removed only after a 2xx, so a failed or half-sent batch is simply
// retried — and because InfluxDB overwrites on identical measurement+tags+timestamp, a
// duplicate retry is harmless rather than a double-count.
bool flushChunk() {
  // Need at least one usable anchor. The previous session's counts too: NTP can be blocked
  // (UDP 123 filtered on a guest network) while HTTPS works fine, and in that case an older
  // anchor is the only thing that makes the backlog deliverable at all.
  if (g_bootEpoch == 0 && g_anchEpoch == 0) return false;

  // Source of truth is the flash log when it mounted, the RAM ring when it didn't.
  static Reading batch[FLUSH_CHUNK];
  int n = 0;
  if (g_fsOk) {
    logCommit();                        // make sure just-taken readings are in the file first
    n = logRead(batch, FLUSH_CHUNK);
  } else {
    n = (rcount < FLUSH_CHUNK) ? rcount : FLUSH_CHUNK;
    memcpy(batch, rbuf, (size_t)n * sizeof(Reading));
  }
  if (n == 0) return false;

  // SSID of the link carrying this batch, as a line-protocol string field. A field (not a
  // tag) keeps series cardinality fixed, and spaces need no escaping inside the quotes.
  // It reflects the network at FLUSH time, which is the one that actually delivered these
  // points — buffered readings may have been taken while offline or on another network.
  String ssid = WiFi.SSID();
  ssid.replace("\\", "\\\\");
  ssid.replace("\"", "\\\"");
  String body;
  body.reserve(n * 200);               // reserved up front so String never doubles mid-build
  String ip = WiFi.localIP().toString();   // where to reach /log.csv on this network
  int sent = 0, undatable = 0;
  for (int i = 0; i < n; i++) {
    uint32_t epoch = resolveEpoch(batch[i]);
    if (epoch == 0) { undatable++; continue; }   // pre-anchor record from a long-dead session
    sent++;
    body += "depth,device=";  body += DEVICE_TAG;
    body += " inches=";       body += String(batch[i].in, 2);
    body += ",raw_cm=";       body += String(batch[i].rawCm, 2);
    body += ",pings_kept=";   body += batch[i].kept;   body += "i";
    body += ",pings_outwin="; body += batch[i].outWin; body += "i";
    body += ",pings_noecho="; body += batch[i].noEcho; body += "i";
    body += ",rssi=";         body += batch[i].rssi;   body += "i";
    body += ",ssid=\"";       body += ssid;           body += "\"";  // which network delivered this batch
    body += ",ip=\"";         body += ip;             body += "\"";  // dashboard shows the /log.csv address
    body += ",fw=";           body += FW_VERSION;         body += "i";   // running firmware version -> Grafana shows which units updated
    body += ",rst=";          body += (int)g_resetReason; body += "i ";  // last reset reason -> annotate reboots (brownout vs crash vs OTA)
    body += epoch;                        // resolved absolute time, back-dating the backlog
    body += "\n";
  }
  if (undatable)
    Serial.printf("FLUSH: %d record(s) from an unanchored earlier session — kept for /log.csv, not sent\n",
                  undatable);

  // Whole chunk was undatable: nothing to POST, but the cursor must still move or delivery
  // would wedge here forever.
  if (sent == 0) {
    if (g_fsOk) logAdvance(n);
    else { rcount -= n; if (rcount > 0) memmove(rbuf, rbuf + n, sizeof(Reading) * rcount); }
    g_pending = (g_pending > (uint32_t)n) ? g_pending - n : 0;
    return true;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, INFLUX_WRITE_URL)) return false;
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(body);
  http.end();

  bool ok = (code >= 200 && code < 300);
  if (ok) {
    if (g_fsOk) logAdvance(n);
    else {
      rcount -= n;
      if (rcount > 0) memmove(rbuf, rbuf + n, sizeof(Reading) * rcount);   // keep oldest-first order
    }
    g_pending = (g_pending > (uint32_t)n) ? g_pending - n : 0;
  } else {
    // 4xx here is usually "points beyond retention period" — a backlog older than the
    // bucket's retention window is rejected outright. Logged loudly because it looks
    // identical to a network failure from the device's side but needs a server-side fix.
    // (The on-device /log.csv download is unaffected by retention, which is part of why
    // it exists.)
    Serial.printf("FLUSH: HTTP %d for %d pts (oldest t=%lu) — retrying\n",
                  code, sent, (unsigned long)resolveEpoch(batch[0]));
  }
  return ok;
}

// Start associating with WIFI_NETS[i] (open network if pass is empty).
void wifiBegin(int i) {
  WiFi.disconnect();
  if (strlen(WIFI_NETS[i].pass) == 0) WiFi.begin(WIFI_NETS[i].ssid);
  else                                WiFi.begin(WIFI_NETS[i].ssid, WIFI_NETS[i].pass);
}

// Blocking: try each network in priority order once (used at boot).
bool connectWifiOnce() {
  for (int i = 0; i < N_WIFI; i++) {
    Serial.printf("WiFi: trying \"%s\" ...\n", WIFI_NETS[i].ssid);
    wifiBegin(i);
    unsigned long t0 = millis();
    while (millis() - t0 < WIFI_ATTEMPT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: connected to \"%s\"  IP %s  RSSI %d\n",
                      WIFI_NETS[i].ssid, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        return true;
      }
      delay(100);
    }
    Serial.printf("WiFi: \"%s\" timed out\n", WIFI_NETS[i].ssid);
  }
  Serial.println("WiFi: no network reachable (will keep retrying)");
  return false;
}

// Non-blocking: when disconnected, cycle through the networks in priority order
// across loop iterations WITHOUT stalling — keeps the local alarm fully responsive
// during an outage. Each network gets WIFI_ATTEMPT_MS before moving to the next.
void ensureWifi() {
  static int idx = 0;
  static unsigned long attemptStart = 0;
  static bool attempting = false;
  if (WiFi.status() == WL_CONNECTED) { attempting = false; return; }
  unsigned long now = millis();
  if (!attempting) {
    Serial.printf("WiFi: re-trying \"%s\" ...\n", WIFI_NETS[idx].ssid);
    wifiBegin(idx);
    attemptStart = now;
    attempting = true;
  } else if (now - attemptStart >= WIFI_ATTEMPT_MS) {
    idx = (idx + 1) % N_WIFI;   // this one didn't take — try the next in order
    attempting = false;
  }
}

// ---------- NTP ----------
void startNtp() { configTime(0, 0, "pool.ntp.org", "time.nist.gov"); }

// Non-blocking. Keeps the clock chasing a real value whenever there's a network. Logging
// no longer depends on it, but DELIVERY does: until the anchor below is set, the backlog
// has no absolute timestamps and stays put.
void serviceNtp() {
  static bool wasUp = false;
  static bool announced = false;
  static unsigned long lastArm = 0;
  bool up     = (WiFi.status() == WL_CONNECTED);
  bool synced = (time(nullptr) > NTP_VALID_EPOCH);
  unsigned long now = millis();

  if (up && !wasUp) {                 // link just came back — re-arm immediately
    Serial.println(F("NTP: WiFi up — re-arming sync"));
    startNtp();
    lastArm = now;
  } else if (up && !synced && now - lastArm >= NTP_REARM_MS) {
    startNtp();                       // still stale; nudge it rather than wait out SNTP's ~1h retry
    lastArm = now;
  }
  wasUp = up;

  // Anchor uptime to wall-clock, once. Everything already buffered becomes datable the
  // instant this lands — including readings taken before any network existed.
  if (synced && g_bootEpoch == 0) {
    g_bootEpoch = (uint32_t)time(nullptr) - uptimeSec();
    // Persist it against this session id: if we reboot before the pre-anchor records have
    // been delivered, the next boot can still date them (see resolveEpoch).
    if (g_fsOk) {
      logPrefs.putUShort("asess",  g_sess);
      logPrefs.putUInt  ("aepoch", g_bootEpoch);
      g_anchSess = g_sess; g_anchEpoch = g_bootEpoch;
    }
    Serial.printf("NTP: clock valid — session %u boot epoch=%lu, %lu buffered reading(s) now datable\n",
                  g_sess, (unsigned long)g_bootEpoch, (unsigned long)logPendingRecords());
  }

  if (synced && !announced) {         // announce once so the serial log shows when delivery began
    Serial.printf("NTP: clock valid (epoch=%lu) — delivery enabled\n", (unsigned long)time(nullptr));
    announced = true;
  }
  if (!synced) announced = false;     // re-announce if we ever lose it
}

// ---------- local history download (HTTP) ----------
// A completely independent way to get the data off the box: no InfluxDB, no token, no
// Caddy, no retention window, no internet. Park outside, share a hotspot, open
// http://drain.local/log.csv (or the IP, which is reported in telemetry as `ip`).
// Deliberately unauthenticated and read-only — it is reachable only by whoever is already
// on the same LAN, and it exposes nothing but water depths.
WebServer server(80);
const char* MDNS_HOST = "drain";

// Advertise on every fresh association: a new network means a new IP, and the old mDNS
// registration is stale.
void serviceMdns() {
  static bool wasUp = false;
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !wasUp) {
    MDNS.end();
    if (MDNS.begin(MDNS_HOST)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("HTTP: http://%s.local/log.csv  (http://%s/log.csv)\n",
                    MDNS_HOST, WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("HTTP: mDNS failed — use http://%s/log.csv\n", WiFi.localIP().toString().c_str());
    }
  }
  wasUp = up;
}

// Streams one log generation as CSV. Sent in blocks with serviceFast() in between, so a
// multi-megabyte download can't leave the buzzer stuck on or the button unresponsive.
void streamLogFile(int idx, uint32_t sinceEpoch) {
  File f = LittleFS.open(LOG_PATH[idx], "r");
  if (!f) return;
  static Reading blk[64];
  char line[160];
  String out;
  out.reserve(sizeof(blk) / LOG_RECSZ * 80);
  while (true) {
    int n = f.read((uint8_t*)blk, sizeof(blk)) / LOG_RECSZ;
    if (n <= 0) break;
    out = "";
    for (int i = 0; i < n; i++) {
      uint32_t ep = resolveEpoch(blk[i]);
      if (sinceEpoch && (ep == 0 || ep < sinceEpoch)) continue;
      char iso[24] = "";
      if (ep) {
        time_t tt = (time_t)ep;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      }
      // Undatable rows are still emitted — unlike the InfluxDB path, a CSV can carry them
      // honestly (blank epoch, uptime filled in), so the shape of an unanchored stretch of
      // history is readable even though its position on the calendar is not.
      unsigned long upCol = (blk[i].flags & RD_RESOLVED) ? 0UL : (unsigned long)blk[i].t;
      // dtostrf, not "%.2f" — float conversion is compiled out of the newlib variant these
      // builds link against, and would silently emit nothing where the depths should be.
      char sIn[12], sRaw[12];
      dtostrf(blk[i].in,    0, 2, sIn);
      dtostrf(blk[i].rawCm, 0, 2, sRaw);
      if (ep) snprintf(line, sizeof(line), "%u,%lu,%lu,%s,%s,%s,%d,%d,%d,%d\n",
                       blk[i].sess, upCol, (unsigned long)ep, iso,
                       sIn, sRaw, blk[i].rssi, blk[i].kept, blk[i].outWin, blk[i].noEcho);
      else    snprintf(line, sizeof(line), "%u,%lu,,,%s,%s,%d,%d,%d,%d\n",
                       blk[i].sess, upCol,
                       sIn, sRaw, blk[i].rssi, blk[i].kept, blk[i].outWin, blk[i].noEcho);
      out += line;
    }
    if (out.length()) server.sendContent(out);
    serviceFast();
  }
  f.close();
}

void handleLogCsv() {
  if (!g_fsOk) { server.send(503, "text/plain", "no filesystem — RAM-only fallback mode\n"); return; }
  uint32_t since = server.hasArg("since") ? (uint32_t)strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
  logCommit();                       // include everything recorded right up to this request

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);   // chunked: total size is not known up front
  server.sendHeader("Content-Disposition", "attachment; filename=\"drain-log.csv\"");
  server.send(200, "text/csv", "");
  server.sendContent(F("session,uptime_s,epoch,iso8601,inches,raw_cm,rssi,pings_kept,pings_outwin,pings_noecho\n"));
  // Oldest generation first: the file we are NOT appending to was written earlier.
  streamLogFile(1 - g_logAct, since);
  streamLogFile(g_logAct, since);
  server.sendContent("");            // zero-length chunk terminates the response
}

void handleRoot() {
  char buf[1200];
  char sLvl[12];
  dtostrf(lastLevelIn, 0, 2, sLvl);      // see streamLogFile: "%f" is not available here
  snprintf(buf, sizeof(buf),
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font:16px system-ui;margin:2rem;max-width:34rem}"
    "td{padding:.2rem .8rem .2rem 0}a{font-size:1.2rem}</style>"
    "<h2>Drain Monitor</h2><table>"
    "<tr><td>Level<td><b>%s in</b>"
    "<tr><td>Firmware<td>v%d"
    "<tr><td>Uptime<td>%lu s (session %u)"
    "<tr><td>Clock<td>%s"
    "<tr><td>Undelivered<td>%lu readings"
    "<tr><td>Log on flash<td>%u + %u bytes"
    "<tr><td>WiFi<td>%s (%d dBm)"
    "</table><p><a href='/log.csv'>Download full history (CSV)</a>",
    sLvl, FW_VERSION, (unsigned long)uptimeSec(), g_sess,
    g_bootEpoch ? "synced" : "not yet synced",
    (unsigned long)logPendingRecords(),
    (unsigned)logFileSize(0), (unsigned)logFileSize(1),
    WiFi.SSID().c_str(), (int)WiFi.RSSI());
  server.send(200, "text/html", buf);
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/log.csv", handleLogCsv);
  server.onNotFound([]() { server.send(404, "text/plain", "try / or /log.csv\n"); });
  server.begin();
}

// ---------- OTA (pull) ----------
// Fetch the manifest; if it advertises a version > FW_VERSION, download that .bin and
// flash it. On success httpUpdate reboots into the new image and this call never returns.
// Blocks for the duration of the download (a few seconds) — the local alarm is frozen
// while it runs, so the caller MUST only invoke this when the water is low (see loop()).
//
// Manifest format (plain text, exactly two lines):
//   <version integer>
//   <https URL of the .bin>
void checkForOTA() {
  // 1) Fetch the manifest.
  WiFiClientSecure mclient;
  mclient.setInsecure();               // encrypt-only, matching the InfluxDB path
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // raw.githubusercontent may 30x
  if (!http.begin(mclient, OTA_MANIFEST_URL)) { Serial.println("OTA: manifest begin failed"); return; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { Serial.printf("OTA: manifest HTTP %d\n", code); http.end(); return; }
  String body = http.getString();
  http.end();

  // 2) Parse version (line 1) and bin URL (line 2).
  int nl = body.indexOf('\n');
  if (nl < 0) { Serial.println("OTA: manifest malformed (need 2 lines)"); return; }
  int  version = body.substring(0, nl).toInt();
  String url   = body.substring(nl + 1);
  url.trim();                          // strip trailing CR / whitespace

  Serial.printf("OTA: running v%d, manifest v%d\n", FW_VERSION, version);
  if (version <= FW_VERSION || url.length() == 0) { Serial.println("OTA: up to date"); return; }

  // 3) Newer version available -> download + flash.
  Serial.printf("OTA: updating -> %s\n", url.c_str());
  digitalWrite(PIN_BUZZER, LOW);       // guarantee silence across the blocking flash

  WiFiClientSecure uclient;
  uclient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub redirects release assets to a CDN host
  httpUpdate.rebootOnUpdate(true);                             // boot the new image on success
  t_httpUpdate_return ret = httpUpdate.update(uclient, url);   // reboots here on success; below only runs on no-op/fail
  switch (ret) {
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: server reported no update");
      break;
    case HTTP_UPDATE_FAILED:
      // Image is verified before the boot partition is switched, so a bad/truncated
      // download leaves the CURRENT firmware running — we just log and carry on.
      Serial.printf("OTA: FAILED (%d) %s\n",
                    httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    default:
      break;
  }
}

// ---------- post-update self-check (network-independent) ----------
// Whether a new binary is "good" is decided LOCALLY, not by whether it reaches Grafana:
// a healthy build at a flaky-WiFi site would be silent on the dashboard, so cloud reach
// is a bad proxy. Instead we watch esp_reset_reason() + an NVS crash counter (NVS survives
// power loss, unlike RTC memory). A build that PANICs / watchdog-loops after an update is
// flagged here with no network involved. BROWNOUT is called out separately — it's a POWER
// fault, not a bad build, so it never counts toward the suspect threshold.
const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "sw-restart";   // our own OTA reboot / watchdog reboot
    case ESP_RST_EXT:       return "ext-reset";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT-WDT";
    case ESP_RST_TASK_WDT:  return "TASK-WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "unknown";
  }
}
static bool isFirmwareCrash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

// Called once, early in setup(): classify how we got here and update the crash counter.
void initSelfCheck() {
  g_bootMs      = millis();
  g_resetReason = esp_reset_reason();
  otaPrefs.begin("ota", false);                 // NVS namespace
  uint32_t lastVer = otaPrefs.getUInt("ver", 0);
  g_bootCount      = otaPrefs.getUInt("boots", 0) + 1;
  g_badBoots       = otaPrefs.getUInt("bad", 0);
  otaPrefs.putUInt("boots", g_bootCount);

  bool firstBootAfterUpdate = (lastVer != (uint32_t)FW_VERSION);

  if (firstBootAfterUpdate) {
    // First time this version has ever run -> record it, start a fresh validation window.
    otaPrefs.putUInt("ver", (uint32_t)FW_VERSION);
    g_badBoots = 0; otaPrefs.putUInt("bad", 0);
    g_pendingValidation = true;
    Serial.printf("SELFCHECK: first boot of fw v%d (was v%lu), reset=%s\n",
                  FW_VERSION, (unsigned long)lastVer, resetReasonStr(g_resetReason));
  } else if (g_resetReason == ESP_RST_BROWNOUT) {
    // Power fault, not a firmware fault — do NOT count it, do NOT re-open validation.
    Serial.printf("SELFCHECK: fw v%d BROWNOUT reset — POWER problem, not the build. "
                  "Check supply / cable / bulk cap.\n", FW_VERSION);
  } else if (isFirmwareCrash(g_resetReason)) {
    // Crashed running an already-seen build -> treat as unproven again and count it.
    g_pendingValidation = true;
    g_badBoots++; otaPrefs.putUInt("bad", g_badBoots);
    Serial.printf("SELFCHECK: fw v%d CRASH (%s) — bad-boot %lu/%d  X\n",
                  FW_VERSION, resetReasonStr(g_resetReason), (unsigned long)g_badBoots, OTA_BAD_BOOT_LIMIT);
  } else {
    Serial.printf("SELFCHECK: fw v%d clean boot (%s)  OK\n", FW_VERSION, resetReasonStr(g_resetReason));
  }

  if (g_badBoots >= (uint32_t)OTA_BAD_BOOT_LIMIT) {
    g_safeMode = true;
    Serial.printf("SELFCHECK: ** SUSPECT BUILD v%d — %lu firmware crashes since last good boot. "
                  "Reflash a known-good build over USB. **\n", FW_VERSION, (unsigned long)g_badBoots);
    Serial.println(F("SELFCHECK: ** SAFE MODE — filesystem and web server disabled. "
                     "Sensor, LEDs, lamp and buzzer still run; telemetry falls back to RAM. **"));
  }
}

// Called every loop: once the firmware has run VALIDATE_STABLE_MS without crashing, mark it
// good and clear the crash counter. This is the network-independent "this binary is OK" gate.
void markValidatedIfStable() {
  if (!g_pendingValidation) return;
  if (millis() - g_bootMs < VALIDATE_STABLE_MS) return;
  g_pendingValidation = false;
  g_badBoots = 0; otaPrefs.putUInt("bad", 0);
  Serial.printf("SELFCHECK: fw v%d validated — stable %lus  OK\n", FW_VERSION, VALIDATE_STABLE_MS / 1000UL);
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(PIN_LED_LG, OUTPUT); pinMode(PIN_LED_HG, OUTPUT);
  pinMode(PIN_LED_LY, OUTPUT); pinMode(PIN_LED_HY, OUTPUT);
  pinMode(PIN_LED_LR, OUTPUT);
  ledcSetup(LAMP_LEDC_CH, LAMP_HZ_ALARM, LAMP_LEDC_BITS);   // overflow lamp: hardware-timed blink
  ledcAttachPin(PIN_LAMP, LAMP_LEDC_CH);
  ledcWrite(LAMP_LEDC_CH, 0);                                // start off
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BTN_LED, OUTPUT); digitalWrite(PIN_BTN_LED, HIGH);  // armed
  digitalWrite(PIN_LED_LG, HIGH);   // alive

  delay(50);
  Serial.println();
  Serial.println(F("St. Joes Drain Monitor — booting"));
  initSelfCheck();   // classify how we booted (clean / crash / brownout) before anything else
  if (!g_safeMode) logInit();   // mount the persistent log BEFORE the first reading is taken
                                // (skipped in safe mode -> g_fsOk stays false -> RAM buffering)

  WiFi.mode(WIFI_STA);
  connectWifiOnce();   // tries SJC guest -> hotspot -> home, in order
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: none up at boot — loop() will keep cycling them");
  }

  startNtp();
  Serial.print("NTP: syncing");
  for (int i = 0; i < 30 && time(nullptr) < NTP_VALID_EPOCH; i++) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.printf("NTP: epoch=%lu\n", (unsigned long)time(nullptr));

  if (!g_safeMode) {
    startWebServer();
    serviceMdns();   // register immediately if WiFi came up during setup
  }

  unsigned long now = millis();
  lastSampleMs = now;
  lastEnqueueMs = now - SLOW_INTERVAL_MS;
  lastSendOkMs = now;
  lastCommitMs = now;
  // First OTA check fires ~OTA_FIRST_CHECK_MS after boot (device sends startup telemetry first),
  // then every OTA_CHECK_INTERVAL_MS.
  lastOtaCheckMs = now - OTA_CHECK_INTERVAL_MS + OTA_FIRST_CHECK_MS;
}

void loop() {
  unsigned long ms = millis();
  serviceFast();       // alarm/button/LEDs responsive at loop rate
  if (!g_safeMode) server.handleClient();   // /log.csv download — cheap when nobody is connected

  if (ms - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = ms;
    ensureWifi();
    serviceNtp();              // re-arm the clock on reconnect — without it, delivery stays gated off
    if (!g_safeMode) serviceMdns();   // (re)advertise drain.local after any reassociation
    serviceLogCommit();        // never leave a staged batch in RAM longer than LOG_COMMIT_MS
    markValidatedIfStable();   // once we've run stably, mark this firmware good (network-independent)

    float lvl = readLevelInches();
    if (!isnan(lvl)) {
      static bool haveLast = false;           // #3 slew limit: clamp jumps vs the last accepted level
      if (haveLast) {
        float dz = lvl - lastLevelIn;
        if (dz >  MAX_SLEW_IN)      lvl = lastLevelIn + MAX_SLEW_IN;
        else if (dz < -MAX_SLEW_IN) lvl = lastLevelIn - MAX_SLEW_IN;
      }
      haveLast = true;
      lastLevelIn = lvl;                       // accepted (slew-limited) value drives indicators AND telemetry
    }

    // ----- telemetry cadence (settle window then 10-min calm) -----
    bool doEnqueue = false;
    const char* regime = "no-echo";
    if (!isnan(lvl)) {
      if (lvl >= TELEMETRY_THRESHOLD) {
        regime = isBelow ? "RISE" : "active";
        isBelow = false;
        doEnqueue = true;
      } else {
        if (!isBelow) { isBelow = true; belowSinceMs = ms; }
        if (ms - belowSinceMs < SETTLE_MS) { regime = "settling"; doEnqueue = true; }
        else { regime = "calm"; if (ms - lastEnqueueMs >= SLOW_INTERVAL_MS) doEnqueue = true; }
      }
    }
    // No clock check here — that gate is exactly what left an offline box with nothing to
    // send. Buffer unconditionally; absolute time is applied later, at flush.
    if (doEnqueue) {
      enqueue(uptimeSec(), lvl, g_rawCm, (int16_t)WiFi.RSSI(),
              (int16_t)g_kept, (int16_t)g_outWin, (int16_t)g_noEcho);
      lastEnqueueMs = ms;
    }

    g_pending = logPendingRecords();     // authoritative refresh, once per second
    uint32_t pending = g_pending;
    Serial.printf("level=%s in  raw=%s cm  [k=%d ow=%d ne=%d]  regime=%s  pend=%lu(%s)  wifi=%s  clk=%s  buz=%d mute=%d ack=%d\n",
                  isnan(lvl) ? "NaN" : String(lvl, 2).c_str(),
                  isnan(g_rawCm) ? "NaN" : String(g_rawCm, 2).c_str(),
                  g_kept, g_outWin, g_noEcho, regime,
                  (unsigned long)pending, g_fsOk ? "flash" : "RAM",
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  g_bootEpoch ? "set" : "unset",
                  (int)beepOn, (int)muted, (int)acked);

    // Steady state: one chunk per tick keeps up easily (we only produce 1 reading/s).
    // A real backlog is drained by the burst path at the bottom of loop().
    if (WiFi.status() == WL_CONNECTED && pending > 0) { if (flushChunk()) lastSendOkMs = ms; }

    // ----- OTA check: infrequent, ONLY when online AND the water is low (never mid-warn/alarm) -----
    // Gating on lastLevelIn < WARN_IN means a reboot-for-update can't take the alarm offline
    // during an actual backup. checkForOTA() blocks a few seconds and reboots on success.
    if (WiFi.status() == WL_CONNECTED &&
        lastLevelIn < WARN_IN &&
        ms - lastOtaCheckMs >= OTA_CHECK_INTERVAL_MS) {
      lastOtaCheckMs = ms;
      logCommit();     // a successful update reboots without returning — don't lose staged readings
      checkForOTA();
    }

    // ----- watchdog: "associated but not delivering", NOT "no network" -----
    // Previously this rebooted after WATCHDOG_MS without a successful send, which at a
    // site whose WiFi is usually absent meant a reboot every 30 minutes — each one wiping
    // the very backlog store-and-forward exists to preserve. Hold the timer open unless
    // we are actually connected AND have something to deliver; then a stalled TLS/HTTP
    // stack is still caught, but simply being out of range is not a fault.
    if (WiFi.status() != WL_CONNECTED || pending == 0) lastSendOkMs = ms;
    if (ms - lastSendOkMs > WATCHDOG_MS) {
      Serial.println("Watchdog reboot");
      logCommit();     // the log outlives the reboot; the staging batch would not
      delay(200);
      ESP.restart();
    }
  }

  // ----- backlog burst drain -----
  // Only while the water is low, mirroring the OTA gate: back-to-back uploads block the
  // loop for their duration, so a backfill is never allowed to compete with an active
  // alarm. serviceFast() at the top of loop() runs between chunks either way.
  if (WiFi.status() == WL_CONNECTED && lastLevelIn < WARN_IN &&
      g_pending > (uint32_t)FLUSH_CHUNK) {
    if (flushChunk()) lastSendOkMs = millis();
    else g_pending = 0;   // stale estimate or a failing send — stop bursting until the next tick
  }

  delay(5);
}
