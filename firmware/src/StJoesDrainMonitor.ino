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
 *   dashboard, with adaptive cadence + NTP timestamps + offline ring buffer.
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
#include <time.h>

#include "secrets.h"   // WIFI_NETS[], N_WIFI, INFLUX_TOKEN — gitignored; see secrets.example.h

// ====== WIFI (networks + priority order live in secrets.h; falls through on failure, then cycles) ======
const unsigned long WIFI_ATTEMPT_MS = 10000UL; // per-network connect timeout

// ====== INFLUXDB (via Caddy HTTPS reverse proxy) ======
const char* INFLUX_WRITE_URL =
    "https://stjoesdrain.eastus.cloudapp.azure.com/api/v2/write?org=stjoes&bucket=drain&precision=s";
// INFLUX_TOKEN is provided by secrets.h (gitignored)
const char* DEVICE_TAG = "drain-1";
// TLS: encrypt but skip cert validation (setInsecure); the write token is the gate.

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

// ====== buffered readings (ring buffer) ======
struct Reading { uint32_t t; float in; float rawCm; int16_t rssi; int16_t kept; int16_t outWin; int16_t noEcho; };
const int BUF_MAX = 2000;
Reading rbuf[BUF_MAX];
int rcount = 0;

// ====== diagnostics from the most recent reading (raw distance + ping quality) ======
float g_rawCm  = NAN;   // median of the in-window pings, in cm (pre-calibration distance)
int   g_kept   = 0;     // pings that landed inside the plausibility window
int   g_outWin = 0;     // pings with an echo but outside the window (gate-rejected)
int   g_noEcho = 0;     // pings with no echo at all

// ====== state ======
unsigned long lastSampleMs = 0;
unsigned long lastEnqueueMs = 0;
unsigned long lastSendOkMs = 0;
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

// ---------- buffer / network ----------
void enqueue(uint32_t t, float in, float rawCm, int16_t rssi, int16_t kept, int16_t outWin, int16_t noEcho) {
  if (rcount >= BUF_MAX) {
    memmove(rbuf, rbuf + 1, sizeof(Reading) * (BUF_MAX - 1));
    rcount = BUF_MAX - 1;
  }
  rbuf[rcount++] = { t, in, rawCm, rssi, kept, outWin, noEcho };
}

bool flushBuffer() {
  if (rcount == 0) return true;
  String body;
  body.reserve(rcount * 96);
  for (int i = 0; i < rcount; i++) {
    body += "depth,device=";  body += DEVICE_TAG;
    body += " inches=";       body += String(rbuf[i].in, 2);
    body += ",raw_cm=";       body += String(rbuf[i].rawCm, 2);
    body += ",pings_kept=";   body += rbuf[i].kept;   body += "i";
    body += ",pings_outwin="; body += rbuf[i].outWin; body += "i";
    body += ",pings_noecho="; body += rbuf[i].noEcho; body += "i";
    body += ",rssi=";         body += rbuf[i].rssi;   body += "i ";
    body += rbuf[i].t;        body += "\n";
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
  if (ok) rcount = 0;
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

  WiFi.mode(WIFI_STA);
  connectWifiOnce();   // tries SJC guest -> hotspot -> home, in order
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: none up at boot — loop() will keep cycling them");
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("NTP: syncing");
  for (int i = 0; i < 30 && time(nullptr) < 1700000000UL; i++) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.printf("NTP: epoch=%lu\n", (unsigned long)time(nullptr));

  unsigned long now = millis();
  lastSampleMs = now;
  lastEnqueueMs = now - SLOW_INTERVAL_MS;
  lastSendOkMs = now;
}

void loop() {
  unsigned long ms = millis();
  serviceFast();   // alarm/button/LEDs responsive at loop rate

  if (ms - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = ms;
    ensureWifi();

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
    if (doEnqueue) {
      uint32_t epoch = (uint32_t)time(nullptr);
      if (epoch > 1700000000UL) {
        enqueue(epoch, lvl, g_rawCm, (int16_t)WiFi.RSSI(),
                (int16_t)g_kept, (int16_t)g_outWin, (int16_t)g_noEcho);
        lastEnqueueMs = ms;
      }
    }

    Serial.printf("level=%s in  raw=%s cm  [k=%d ow=%d ne=%d]  regime=%s  buf=%d  wifi=%s  buz=%d mute=%d ack=%d\n",
                  isnan(lvl) ? "NaN" : String(lvl, 2).c_str(),
                  isnan(g_rawCm) ? "NaN" : String(g_rawCm, 2).c_str(),
                  g_kept, g_outWin, g_noEcho, regime, rcount,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  (int)beepOn, (int)muted, (int)acked);

    if (WiFi.status() == WL_CONNECTED && rcount > 0) { if (flushBuffer()) lastSendOkMs = ms; }
    if (millis() - lastSendOkMs > WATCHDOG_MS) { Serial.println("Watchdog reboot"); delay(200); ESP.restart(); }
  }

  delay(5);
}
