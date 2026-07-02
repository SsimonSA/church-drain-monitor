/*
 * Interactive bench-test menu — St. Joes Drain Monitor bring-up
 * ---------------------------------------------------------------------------
 * Exercises each subsystem ONE keypress at a time so a fault is isolated to a
 * single component. Type the menu key in the serial monitor (no Enter needed —
 * `pio device monitor` sends keystrokes immediately).
 *
 * Pin map + calibration copied verbatim from the main firmware so this tests
 * the REAL wiring, not a guess.
 *
 *   1  LED bar walk      — light each of the 5 segments in turn (GPIO 18/5/17/16/4)
 *   2  LED bar all-on    — all 5 segments on for 2 s
 *   o  Overflow lamp     — blink the red lamp 5x (GPIO 2)
 *   b  Button watch      — print press / short / long-press events (GPIO 32); 'x' exits
 *   r  Ring LED toggle   — flip the button's ring LED on/off (GPIO 25 -> MOSFET)
 *   z  Buzzer blip       — one short beep (GPIO 27 -> MOSFET)  ** LOUD: 95 dB **
 *   s  Sensor read       — continuous SR04 raw cm + calibrated inches; 'x' exits
 *   w  WiFi join         — try the 3 networks in order, print IP/RSSI
 *   t  Cloud write       — one test point to InfluxDB (needs WiFi up first)
 *   h  Help              — reprint this menu
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "secrets.h"   // WIFI_NETS[], N_WIFI, INFLUX_TOKEN — gitignored; see secrets.example.h
#include <time.h>

// ===== GPIO MAP (matches main firmware) =====
#define PIN_LED_LG 18   // green  (lowest bar segment)
#define PIN_LED_HG 5    // green
#define PIN_LED_LY 17   // yellow
#define PIN_LED_HY 16   // yellow
#define PIN_LED_LR 4    // red    (highest bar segment)
#define PIN_LAMP   2    // overflow lamp
#define PIN_BUZZER 27   // active piezo via N-MOSFET on the 12V rail
#define PIN_BUTTON 32   // momentary, to GND, INPUT_PULLUP
#define PIN_BTN_LED 25  // ring LED via N-MOSFET on the 12V rail, active-HIGH

const int TRIG_PIN = 22;
const int ECHO_PIN = 21;

// ===== SR04 + calibration (from main firmware) =====
const int   SAMPLES = 21;
const unsigned long ECHO_TIMEOUT_US = 30000UL;
const int   PING_GAP_MS = 8;
const float US_PER_CM = 58.0f;
const float TUBE_LEN_CM  = 30.7f;
const float FLOAT_LEN_CM = 4.0f;
const float CM_PER_IN    = 2.54f;
const float CAL_RAW_LO   = 0.43f;
const float CAL_TRUE_LO  = 0.00f;
const float CAL_RAW_HI   = 4.909f;
const float CAL_TRUE_HI  = 5.00f;
const float ZERO_OFFSET_IN = 1.27f;   // 0.42 -> 0.84 (tube shortened) -> 1.27 (+0.43, re-zero for new float/press-fit cap)

// ===== WiFi / InfluxDB (WIFI_NETS[], N_WIFI, INFLUX_TOKEN come from secrets.h) =====
const unsigned long WIFI_ATTEMPT_MS = 10000UL;
const char* INFLUX_WRITE_URL =
    "https://stjoesdrain.eastus.cloudapp.azure.com/api/v2/write?org=stjoes&bucket=drain&precision=s";
const char* DEVICE_TAG = "drain-1";

const int LED_PINS[5] = { PIN_LED_LG, PIN_LED_HG, PIN_LED_LY, PIN_LED_HY, PIN_LED_LR };
const char* LED_NAMES[5] = { "LG green", "HG green", "LY yellow", "HY yellow", "LR red" };

// Defined up here (before any function) so the auto-generated .ino prototypes
// that return it are valid.
struct SensorRead {
  float medianCm;   // median of the IN-WINDOW pings, or -1 if none survived
  float inches;     // calibrated level for that median, or NAN
  int   kept;       // pings inside the plausibility window
  int   outWin;     // pings with an echo but outside the window (rejected by #1)
  int   noEcho;     // pings with no echo at all
};

// ---------- helpers ----------
void allLedsOff() {
  for (int i = 0; i < 5; i++) digitalWrite(LED_PINS[i], LOW);
}

// true if the user pressed a key (used to break out of continuous modes)
bool keyPressed() { return Serial.available() > 0; }

float pingOnce() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1.0f;
  return us / US_PER_CM;
}

void sortAsc(float *a, int n) {
  for (int i = 1; i < n; i++) {
    float key = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
}

// ===== ROBUSTNESS TUNING (bench) =====
// #1 PLAUSIBILITY GATE — the float echo must land inside this one-way distance
//    band. Anything NEARER (wall / rim / cross-talk) or FARTHER (no float) is
//    discarded BEFORE the median, so a spurious 4 cm echo can't pull the reading
//    short. These are deliberately generous for now (tube is 30.7 cm long); they
//    get tightened to the real float-travel band at calibration.
const float DIST_MIN_CM = 10.0f;
const float DIST_MAX_CM = 30.0f;
// #3 SLEW LIMIT — real water can't jump this many inches between samples, so a
//    larger jump is clamped to this step. One stray sample can't spike the level.
const float MAX_SLEW_IN = 1.5f;

float cmToInches(float median) {
  float rawIn = (TUBE_LEN_CM - (median + FLOAT_LEN_CM)) / CM_PER_IN;
  float slope = (CAL_TRUE_HI - CAL_TRUE_LO) / (CAL_RAW_HI - CAL_RAW_LO);
  float levelIn = CAL_TRUE_LO + (rawIn - CAL_RAW_LO) * slope - ZERO_OFFSET_IN;
  if (levelIn < 0.0f) levelIn = 0.0f;
  return levelIn;
}

// Burst of SAMPLES pings, keeping only echoes inside the plausibility window (#1),
// then median those. Reports the kept/out-of-window/no-echo breakdown so you can
// SEE the gate rejecting junk live.
SensorRead readSensorGated() {
  static float buf[SAMPLES];
  int kept = 0, outWin = 0, noEcho = 0;
  for (int i = 0; i < SAMPLES; i++) {
    float d = pingOnce();
    if (d <= 0)                                  noEcho++;
    else if (d < DIST_MIN_CM || d > DIST_MAX_CM) outWin++;     // #1 reject
    else                                         buf[kept++] = d;
    delay(PING_GAP_MS);
  }
  SensorRead r;
  r.kept = kept; r.outWin = outWin; r.noEcho = noEcho;
  if (kept == 0) { r.medianCm = -1.0f; r.inches = NAN; return r; }
  sortAsc(buf, kept);
  r.medianCm = buf[kept / 2];
  r.inches   = cmToInches(r.medianCm);
  return r;
}

// ---------- tests ----------
void testLedWalk() {
  Serial.println(F("[1] LED bar walk — each segment 600 ms"));
  allLedsOff();
  for (int i = 0; i < 5; i++) {
    Serial.printf("    seg %d: %s (GPIO %d)\n", i + 1, LED_NAMES[i], LED_PINS[i]);
    digitalWrite(LED_PINS[i], HIGH);
    delay(600);
    digitalWrite(LED_PINS[i], LOW);
  }
  Serial.println(F("    done."));
}

void testLedAll() {
  Serial.println(F("[2] LED bar all-on for 2 s"));
  for (int i = 0; i < 5; i++) digitalWrite(LED_PINS[i], HIGH);
  delay(2000);
  allLedsOff();
  Serial.println(F("    done."));
}

void testLamp() {
  Serial.println(F("[o] Overflow lamp — 5 blinks"));
  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_LAMP, HIGH); delay(300);
    digitalWrite(PIN_LAMP, LOW);  delay(300);
  }
  Serial.println(F("    done."));
}

void testButton() {
  Serial.println(F("[b] Button watch — press the button; 'x' to exit"));
  bool stable = HIGH, lastRaw = HIGH, longFired = false;
  unsigned long lastChange = 0, pressStart = 0;
  const unsigned long DEBOUNCE_MS = 30UL, LONG_PRESS_MS = 2000UL;
  while (true) {
    if (keyPressed() && Serial.read() == 'x') { Serial.println(F("    exit.")); return; }
    unsigned long ms = millis();
    bool raw = digitalRead(PIN_BUTTON);
    if (raw != lastRaw) { lastRaw = raw; lastChange = ms; }
    if (ms - lastChange > DEBOUNCE_MS && raw != stable) {
      stable = raw;
      if (stable == LOW) { pressStart = ms; longFired = false; Serial.println(F("    press")); }
      else { if (!longFired) Serial.println(F("    -> SHORT press (acknowledge)")); }
    }
    if (stable == LOW && !longFired && (ms - pressStart >= LONG_PRESS_MS)) {
      longFired = true; Serial.println(F("    -> LONG press (mute toggle)"));
    }
  }
}

bool ringLedState = false;
void testRingLed() {
  ringLedState = !ringLedState;
  digitalWrite(PIN_BTN_LED, ringLedState);
  Serial.printf("[r] Ring LED = %s\n", ringLedState ? "ON" : "off");
}

void testBuzzer() {
  Serial.println(F("[z] Buzzer blip (120 ms) ** LOUD **"));
  digitalWrite(PIN_BUZZER, HIGH); delay(120);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println(F("    done."));
}

void testSensor() {
  Serial.println(F("[s] Sensor read — gated (#1) + slew-limited (#3); 'x' to exit"));
  Serial.printf("    gate = [%.1f .. %.1f] cm   slew <= %.2f in/sample\n",
                DIST_MIN_CM, DIST_MAX_CM, MAX_SLEW_IN);
  float lastIn = NAN;   // last accepted level, for the slew limiter
  while (true) {
    if (keyPressed() && Serial.read() == 'x') { Serial.println(F("    exit.")); return; }
    SensorRead r = readSensorGated();

    if (r.kept == 0) {   // every ping was junk — hold the last good level
      Serial.printf("    NO VALID PING  [kept=0 outWin=%d noEcho=%d]  holding %.2f in\n",
                    r.outWin, r.noEcho, isnan(lastIn) ? 0.0f : lastIn);
      delay(400);
      continue;
    }

    // #3 slew limit: clamp a too-large jump to MAX_SLEW_IN so a stray sample
    //    can't spike the reported level.
    float accepted = r.inches;
    const char* flag = "";
    if (!isnan(lastIn)) {
      float dz = r.inches - lastIn;
      if (dz > MAX_SLEW_IN)       { accepted = lastIn + MAX_SLEW_IN; flag = "  [slew-clamp up]"; }
      else if (dz < -MAX_SLEW_IN) { accepted = lastIn - MAX_SLEW_IN; flag = "  [slew-clamp dn]"; }
    }
    lastIn = accepted;

    Serial.printf("    raw=%.2fcm  level=%.2fin  accepted=%.2fin  [kept=%d outWin=%d noEcho=%d]%s\n",
                  r.medianCm, r.inches, accepted, r.kept, r.outWin, r.noEcho, flag);
    delay(400);
  }
}

void testWifi() {
  Serial.println(F("[w] WiFi join — trying networks in order"));
  WiFi.mode(WIFI_STA);
  for (int i = 0; i < N_WIFI; i++) {
    Serial.printf("    trying \"%s\" ...\n", WIFI_NETS[i].ssid);
    WiFi.disconnect();
    if (strlen(WIFI_NETS[i].pass) == 0) WiFi.begin(WIFI_NETS[i].ssid);
    else                                WiFi.begin(WIFI_NETS[i].ssid, WIFI_NETS[i].pass);
    unsigned long t0 = millis();
    while (millis() - t0 < WIFI_ATTEMPT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("    CONNECTED  IP %s  RSSI %d\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        return;
      }
      delay(100);
    }
    Serial.println(F("    timed out"));
  }
  Serial.println(F("    no network reachable"));
}

void testCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[t] Cloud write — WiFi not up. Run 'w' first."));
    return;
  }
  Serial.println(F("[t] Cloud write — posting a test point (inches=1.23)"));
  String body = "depth,device=";
  body += DEVICE_TAG; body += " inches=1.23,rssi="; body += (int)WiFi.RSSI(); body += "i";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  if (!http.begin(client, INFLUX_WRITE_URL)) { Serial.println(F("    http.begin failed")); return; }
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(body);
  http.end();
  Serial.printf("    HTTP %d  (204 = success)\n", code);
}

void printMenu() {
  Serial.println();
  Serial.println(F("===== Drain Monitor — Bench Test Menu ====="));
  Serial.println(F("  1  LED bar walk        2  LED bar all-on"));
  Serial.println(F("  o  Overflow lamp       b  Button watch ('x' exits)"));
  Serial.println(F("  r  Ring LED toggle     z  Buzzer blip (LOUD)"));
  Serial.println(F("  s  Sensor read ('x' exits)"));
  Serial.println(F("  w  WiFi join           t  Cloud write"));
  Serial.println(F("  h  Reprint this menu"));
  Serial.println(F("==========================================="));
  Serial.print(F("> "));
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT); digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);
  for (int i = 0; i < 5; i++) { pinMode(LED_PINS[i], OUTPUT); digitalWrite(LED_PINS[i], LOW); }
  pinMode(PIN_LAMP, OUTPUT);   digitalWrite(PIN_LAMP, LOW);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN_LED, OUTPUT);digitalWrite(PIN_BTN_LED, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  delay(50);
  Serial.println();
  Serial.println(F("Bench test ready. All outputs start OFF."));
  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == '\n' || c == '\r' || c == ' ') return;   // ignore whitespace
  switch (c) {
    case '1': testLedWalk();  break;
    case '2': testLedAll();   break;
    case 'o': testLamp();     break;
    case 'b': testButton();   break;
    case 'r': testRingLed();  break;
    case 'z': testBuzzer();   break;
    case 's': testSensor();   break;
    case 'w': testWifi();     break;
    case 't': testCloud();    break;
    case 'h': case '?': printMenu(); return;
    default:  Serial.printf("    unknown key '%c' — press h for menu\n", c); break;
  }
  Serial.print(F("> "));
}
