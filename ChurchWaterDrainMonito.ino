#include <Wire.h>
#include <VL53L1X.h>  // Using Pololu VL53L1X library for ROI control

VL53L1X vl53;

// LED pins
#define LGLED 18
#define HGLED 5
#define LYLED 17
#define HYLED 16 
#define LRLED 4
const int LED_OVERFLOW = 2;

// Drain measurements
const int SENSOR_HEIGHT_MM = 381;  
const int DRAIN_DEPTH_MM = 60;
int emptyBaseline = SENSOR_HEIGHT_MM;

// Smoothing (SMA)
const int SMA_WINDOW = 15; // A smaller window for better responsiveness
int smaBuffer[SMA_WINDOW];
int smaIndex = 0;
long smaSum = 0; // Use a running sum for efficiency
int lastFilteredDistance = 0;

// Overflow LED blinking
unsigned long lastBlink = 0;
bool blinkState = false;
const unsigned long BLINK_INTERVAL = 200;

// Function to update the bar graph LEDs
void updateBarGraph(float percent) {
    digitalWrite(LGLED, percent >= 0);
    digitalWrite(HGLED, percent >= 15);
    digitalWrite(LYLED, percent >= 30);
    digitalWrite(HYLED, percent >= 45);
    digitalWrite(LRLED, percent >= 60);
}

// Function to handle the blinking overflow LED
void handleOverflowLED(float percent) {
    if (percent >= 80) {
        if (millis() - lastBlink >= BLINK_INTERVAL) {
            lastBlink = millis();
            blinkState = !blinkState;
            digitalWrite(LED_OVERFLOW, blinkState);
        }
    } else {
        // If not in overflow state, ensure the LED is off and reset blink state
        blinkState = false;
        digitalWrite(LED_OVERFLOW, LOW);
    }
}

// Efficient SMA function using a running sum
int smoothSMA(int newVal) {
    // Subtract the oldest value from the sum
    smaSum -= smaBuffer[smaIndex];
    // Add the new value to the buffer and the sum
    smaBuffer[smaIndex] = newVal;
    smaSum += newVal;
    // Advance the index
    smaIndex = (smaIndex + 1) % SMA_WINDOW;
    // Return the average
    return smaSum / SMA_WINDOW;
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
    vl53.setTimeout(500);
    if (!vl53.init()) {
        Serial.println("Failed to detect VL53L1X sensor");
        while (1);
    }
    Serial.println("VL53L1X initialized (Pololu).");

    // Sensor configuration
    vl53.setROISize(4, 4);
    vl53.setROICenter(199);
    vl53.setDistanceMode(VL53L1X::Long);
    vl53.setMeasurementTimingBudget(140000);
    vl53.startContinuous(140);

    // Configure LEDs
    pinMode(LGLED, OUTPUT);
    pinMode(HGLED, OUTPUT);
    pinMode(LYLED, OUTPUT);
    pinMode(HYLED, OUTPUT);
    pinMode(LRLED, OUTPUT);
    pinMode(LED_OVERFLOW, OUTPUT);

    // Auto-calibrate baseline
    Serial.println("Calibrating empty baseline...");
    long sum = 0;
    const int samples = 20;
    int validSamples = 0;
    for (int i = 0; i < samples; i++) {
        int d = vl53.read();
        if (vl53.timeoutOccurred()) { Serial.print(" TIMEOUT"); }
        if (d > 0) {
            sum += d;
            validSamples++;
        }
        delay(150); // Delay slightly longer than inter-measurement time
    }

    if (validSamples > 0) {
        emptyBaseline = sum / validSamples;
    }

    // Pre-fill the SMA buffer with the baseline to avoid initial drop
    for (int i = 0; i < SMA_WINDOW; i++) {
        smaBuffer[i] = emptyBaseline;
    }
    smaSum = (long)emptyBaseline * SMA_WINDOW;
    lastFilteredDistance = emptyBaseline;

    Serial.print("Baseline (empty drain): ");
    Serial.println(emptyBaseline);
}

void loop() {
    // 1. Check for new data and update the filtered distance
    if (vl53.dataReady()) {
        int raw = vl53.read();
        // Only use valid readings
        if (raw > 0) {
            lastFilteredDistance = smoothSMA(raw);
        }
    }
    
    // 2. Calculate water height and percentage from the latest filtered value
    int waterHeight = emptyBaseline - lastFilteredDistance;
    if (waterHeight < 0) waterHeight = 0;
    if (waterHeight > DRAIN_DEPTH_MM) waterHeight = DRAIN_DEPTH_MM;
    float percent = (float)waterHeight / DRAIN_DEPTH_MM * 100.0;

    // 3. Update all LEDs on every loop for maximum responsiveness
    updateBarGraph(percent);
    handleOverflowLED(percent);

    // Optional: Print status periodically, not on every loop, to avoid spamming the serial monitor.
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 1000) {
      lastPrint = millis();
      Serial.print("Filtered Distance: "); Serial.print(lastFilteredDistance);
      Serial.print(" | Water Height: "); Serial.print(waterHeight);
      Serial.print(" mm | Level: "); Serial.print(percent, 1);
      Serial.println(" %");
    }
}