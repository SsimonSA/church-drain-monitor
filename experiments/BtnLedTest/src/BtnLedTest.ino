/*
 * Bench test #1 — Button ring LED
 * ---------------------------------------------------------------------------
 * Blinks the illuminated push button's ring LED once per second.
 *
 * Exercises the whole new low-side driver chain in one shot:
 *     GPIO 25  ->  220R  ->  2N7000 gate  ->  drain  ->  LED-  ...  LED+ -> 12V rail
 * If the LED blinks, then: the MOSFET switches, the 100k pulldown isn't
 * shorting the gate, the 12V boost rail is live, and GPIO 25 is the right pin.
 *
 * Nothing else is driven. Buzzer (GPIO 27) and the LED bar are left alone.
 * Serial prints the state each toggle so you can confirm even if you can't
 * see the LED from the keyboard.
 */

#define PIN_BTN_LED 25            // 12V ring LED via N-MOSFET low-side switch, active-HIGH

const unsigned long BLINK_MS = 1000UL;   // 1 s on, 1 s off — slow and obvious

bool ledOn = false;
unsigned long lastToggle = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_LED, OUTPUT);
  digitalWrite(PIN_BTN_LED, LOW);   // start OFF
  delay(50);
  Serial.println();
  Serial.println(F("Bench test #1 — button ring LED on GPIO 25"));
  Serial.println(F("Expect: LED blinks 1s on / 1s off. Watch the button."));
}

void loop() {
  if (millis() - lastToggle >= BLINK_MS) {
    lastToggle = millis();
    ledOn = !ledOn;
    digitalWrite(PIN_BTN_LED, ledOn);
    Serial.printf("ring LED = %s\n", ledOn ? "ON" : "off");
  }
}
