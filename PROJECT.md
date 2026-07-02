# 🚰 Drain Overflow Monitor

Visual early-warning system for a church kitchen floor drain that occasionally backs up
under a wash sink. Goal: warn **before** overflow, in a wet/dirty/greasy environment.

## The problem

- Floor drain begins **overflowing at ~5.5"** of water depth.
- Environment is hostile to sensors: wet, dirty, soap, grease, food residue, scum.
- Need a visual warning that escalates as water rises, flashing before overflow.

### The drain site (photographed)

- Commercial **enameled cast-iron floor sink**, roughly **~12" square opening**, **5.5" deep**,
  with a **dome/flat strainer at the bottom**.
- It's an **indirect-waste receptor**: several vertical PVC pipes plus a horizontal one discharge
  *into* the basin. It backs up when inflow outruns the bottom drain, overflowing at 5.5"
  (flush with the floor).
- The basin is **crowded with existing pipes** — a float standpipe must tuck into a corner,
  rest on the bottom, and have intake holes low so it tracks the basin water level.

## Display (already built)

The enclosure is built and the LED layout is finalized:

- **5 level LEDs:** Green, Green, Yellow, Yellow, Red
- **1 large red LED** that flashes when overflow is imminent

| Water depth | Indication |
|-------------|------------|
| 0"   | 1st green LED |
| ~1"  | 2 LEDs |
| ~2"  | 3 LEDs |
| ~3"  | 4 LEDs |
| ~3.5"| 5 LEDs |
| 4"   | Large red LED begins flashing |
| 5.5" | Actual overflow begins |

Only need to measure **0"–4"** before warning.

### As-built display unit (photographed)

- 3D-printed two-part enclosure, hex-textured lid: **"DRAIN MONITOR / OVERFLOW WARNING / WATER LEVEL"**, clamp-mounted.
- **ESP32-WROOM-32U** DevKit on a red-framed perfboard.
- 5 bar LEDs (G, G, Y, Y, R) wired through a row of current-limiting resistors; one large **panel-mount red overflow indicator** up top.
- **Toggle power switch** in the box; USB-powered.
- **JST connectors** for the detachable lid-LED harness and for the sensor cable; sensor cable uses an inline waterproof bullet connector.

Current firmware pin map (from the legacy `.ino` — **verify against actual wiring** before reuse):
LGLED=18, HGLED=5, LYLED=17, HYLED=16, LRLED=4, overflow LED=2. (The panel-mount overflow indicator may have a built-in resistor — only ~5 board resistors are visible, matching the 5 bar LEDs.)

## Sensing approach — history

1. **JSN-SR04T ultrasonic** — ❌ Beam spread too wide, drain-wall reflections, splashing, dirt. Unreliable.
2. **VL53L1X ToF (TOF400C modules)** — explored FOV, ROI reduction, Long mode, timing budgets,
   smoothing, auto-calibration. Better than ultrasonic but ❌ still untrustworthy: splashes,
   dirty/changing reflectivity, scum accumulation.
   - *The existing `ChurchWaterDrainMonito.ino` is this abandoned VL53L1X version.*
3. **Float + Hall sensors** — keeps electronics dry, easy to clean, immune to splashes/optics.
   Designed but **set aside** (see "Alternative / fallback" below) in favor of #4.
4. **Float + HC-SR04 in a pipe (CURRENT direction)** — return to the HC-SR04, but fix its old
   failure modes *mechanically*: a float in a 2" PVC pipe gives the ultrasonic a clean, flat,
   consistent target, and the pipe bore acts as a waveguide that collimates the beam (no more
   drain-wall reflections or splashing). Plays to the SR04's strength at short range. 🔬 Under test.

## Current direction: float + HC-SR04 in a 2" PVC pipe

- **2" PVC pipe** stands vertically in the drain; an **HC-SR04 in a 3D-printed end cap** sits at
  the **top**, aimed down the bore.
- A **3D-printed float** rides on the water surface inside the pipe. The SR04 measures distance to
  the **flat top of the float** → water level = pipe geometry − measured distance.
- The pipe shields the float from splashing/turbulence and collimates the ultrasonic beam.

**Geometry (test rig):** pipe **12" long**, float **~1.5" long**. The float is *closest* to the
sensor at overflow and *farther* as water drops, so:
- At overflow (~5.5" water), sensor-to-float-top ≈ **5"** — comfortably outside the SR04 dead zone.
- The near dead zone (~2 cm, ring-down) is therefore **never reached** in this geometry. The bench
  sweep confirms accuracy/linearity/repeatability over the full travel rather than chasing a dead zone.

### Bench test rig (built, photographed)

- 2" PVC pipe + 3D-printed cap holding the HC-SR04 at one end, on a breadboard ESP32.
- The black float is **hot-glued to a pencil** as a manual actuator: push the float to a known
  position (read off a tape measure at the open end) and compare to the SR04 distance reading.
- Goal: characterize **known float position vs. measured distance** — accuracy, linearity,
  repeatability, and where the near dead zone begins.

## Alternative / fallback: float inside PVC pipe (Hall-sensor version)

- Vertical **1.5" Schedule 40 PVC** pipe; water enters through holes near the bottom.
- A **float** rides up/down inside the pipe with a **magnet** attached.
- **Analog Hall-effect sensors mounted outside the pipe** read the magnet's field.

### Sensors

- Purchased **10x DRV5055A1QLPG** — analog Hall-effect, output proportional to field strength,
  good for interpolation.
- Sensor count strategy evolved: 5 → 3 with interpolation. Current thinking: **3 sensors** may
  suffice for the 0"–4" range, e.g. Sensor 1 = 0", Sensor 2 = 2", Sensor 3 = 4". Magnet field
  overlaps adjacent sensors; interpolate all 3 analog values to estimate float position
  continuously, then map to the LED display.

### Float design

- **38 mm OD × 50 mm long**, pill-shaped, fits inside 1.5" PVC (ID ≈ 41 mm → ~1.5 mm clearance
  per side, tolerant of scum/buildup/print imperfection).
- **Bottom half:** rounded bottom, hollow chamber, magnet pocket.
- **Top half:** M32×2 threaded cap, domed top, tool slot.
- **Thread clearance:** female 32.3 mm / male 31.7 mm (0.6 mm total, 0.3 mm/side) — good FDM start.
- **Magnet:** 10 mm dia × 2.7 mm thick neodymium, in bottom of float, epoxy-sealed. Serves as
  Hall target **and** ballast (keeps float oriented).
- **Buoyancy:** hollow PLA float with a **Styrofoam insert** — stays buoyant even if PLA leaks,
  no perfect seal required.

## Audible alarm (new)

- Adding a **piezo buzzer**: WEICHUANG **SFM-27-W**, DC **3–24 V**, **90 dB**, *active* (self-oscillating,
  continuous tone — just apply DC, no PWM/tone needed). Arriving 2026-05-29.
- **Intended trigger: ~1" below overflow ≈ 4.5" water depth** — between the big-red-LED flashing
  onset (4") and actual overflow (5.5"). Final threshold deferred until the distance experiments
  are done.
- **Drive note:** an active buzzer rated to 24 V can draw more current than an ESP32 GPIO should
  source directly — plan to drive it through a transistor/MOSFET (or confirm draw at 3.3–5 V first).

## Status

**Mechanical:** ✅ enclosure built · ✅ LED layout finalized · ✅ magnet selected · ✅ Hall sensors
ordered · float design underway · threaded cap modeled · 1.5" PVC selected.

**Electronics:** ✅ ESP32 on hand · ✅ DRV5055 sensors purchased. Need: sensor mounting
arrangement, calibration strategy, final interpolation algorithm.

**Software (Hall version):** Not started. Need to: read 3 DRV5055 analog sensors → smooth →
calculate float position → convert to water depth → drive LEDs → flash overflow LED at 4".

## Next major milestone

Build the float, install it in a piece of 1.5" PVC, and **characterize magnetic field vs. height
using one DRV5055 sensor** before committing to final sensor spacing. That experiment determines
how many sensors are actually needed.

The **old ultrasonic test rig** (PVC tube + sensor in a printed end-cap + tape-measure position
reference + breadboard ESP32) is a ready template for this characterization: slide the
magnet/float along the pipe, log DRV5055 analog reading vs. tape-measure position.
