#include <NewPing.h>

#define TRIG_PIN 5
#define ECHO_PIN 18
#define MAX_DISTANCE 400  // Maximum sensor distance in cm
#define LGLED 2
#define HGLED 4
#define LYLED 16
#define HYLED 17 
#define LRLED 19
#define HRLED 21
NewPing sonar(TRIG_PIN, ECHO_PIN, MAX_DISTANCE);

void setup() {
  Serial.begin(115200);
  pinMode(LGLED, OUTPUT);
  pinMode(HGLED, OUTPUT);
  pinMode(LYLED, OUTPUT);
  pinMode(HYLED, OUTPUT);
  pinMode(LRLED, OUTPUT);
  pinMode(HRLED, OUTPUT);
}

void loop() {
  delay(100); // Short delay before measurement
  int distance = sonar.ping_cm();  // Get distance in cm

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  if (distance < 31) {
    digitalWrite(LGLED, HIGH); // Turn on LED
  } else {
    digitalWrite(LGLED, LOW);  // Turn off LED
  }

  if (distance < 30) {
    digitalWrite(HGLED, HIGH); // Turn on LED
  } else {
    digitalWrite(HGLED, LOW);  // Turn off LED
  }

    if (distance < 29) {
    digitalWrite(LYLED, HIGH); // Turn on LED
  } else {
    digitalWrite(LYLED, LOW);  // Turn off LED
  }

      if (distance < 28) {
    digitalWrite(HYLED, HIGH); // Turn on LED
  } else {
    digitalWrite(HYLED, LOW);  // Turn off LED
  }

    if (distance < 27) {
    digitalWrite(LRLED, HIGH); // Turn on LED
  } else {
    digitalWrite(LRLED, LOW);  // Turn off LED
  }

if (distance < 26) {
  digitalWrite(HRLED, HIGH);
  delay(100);
  digitalWrite(HRLED, LOW);
  delay(100);
} else {
  digitalWrite(HRLED, LOW);
}
  delay(500); // Slow down loop
}
