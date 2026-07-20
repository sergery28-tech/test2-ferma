# FERMA — ESP32 Soil NPK Dashboard & Dispenser

An ESP32-based field tool that reads soil N/P/K, computes a per-plant
fertilizer dose, and dispenses it through servo-controlled gates — one
nutrient at a time — triggered by a physical button on the device.

The ESP32 hosts its own web dashboard (no cloud, no app install): connect
to the same WiFi router as the device and open its IP address in a browser
to see live status, a running log of every plant treated, and a nutrient
trend chart.

## How it works

1. Carry the device to a plant and press the button.
2. The ESP32 reads the soil sensor, computes a dose for N, P, and K, and
   opens each gate in sequence (N → P → K) for a calculated duration.
3. The reading and the amounts dispensed are logged in memory under an
   incrementing plant number.
4. The device enters a short cooldown so a stray press while walking to
   the next plant can't trigger a double dose.
5. The dashboard (served by the ESP32 itself) polls the device for status
   and new plant records and updates automatically — no interaction
   required to actually dispense.

## Hardware

- ESP32 dev board
- Push button (wired to `BUTTON_PIN`, other side to GND — uses the
  internal pull-up, no external resistor needed)
- 3× servo-controlled gates, one per nutrient (`SERVO_PIN_N/P/K`)
- Soil NPK sensor (not yet wired in this version — see below)

## Live demo

`index.html` at the root of this repo is a fully self-contained demo —
open it directly (or via GitHub Pages) and it simulates the whole
button-press-to-dispense flow in the browser, no ESP32 required. It's
there so you can try the workflow before building the hardware.

## Repo layout

```
index.html               # Standalone demo (GitHub Pages entry point)
ferma_dashboard/
  ferma_dashboard.ino     # ESP32 firmware
  data/
    index.html            # Real dashboard served by the ESP32 (LittleFS)
```

## Setup

1. Install the **ESP32** board package in Arduino IDE (Boards Manager).
2. Install these libraries (Library Manager):
   - `ESP32Servo` (Kevin Harrington / madhephaestus)
   - `ArduinoJson` (Benoit Blanchon, v6 or v7)
3. Install the LittleFS filesystem uploader plugin for Arduino IDE (or use
   `arduino-cli` / PlatformIO's LittleFS upload feature).
4. Open `ferma_dashboard.ino` and set `WIFI_SSID` / `WIFI_PASSWORD`.
5. Use "ESP32 Sketch Data Upload" to flash `data/index.html` onto the
   board's LittleFS partition.
6. Upload the sketch normally.
7. Open the Serial Monitor at 115200 baud — it prints the device's IP
   address once WiFi connects. On any device on the same WiFi, open that
   IP in a browser.

## Calibration (do this before trusting the numbers)

- **Dose model** — `RECO_MODEL` in `ferma_dashboard.ino` (target index +
  max grams per plant) must match `RECO_MODEL` in `data/index.html`. They
  are kept in sync manually; if you change one, change the other.
- **Flow rate** — `FLOW_RATE_G_PER_SEC_N/P/K` are placeholder guesses.
  To calibrate: open a gate for a fixed time over an empty container,
  weigh what came out, then set `FLOW_RATE_G_PER_SEC = grams / seconds`.
  Recheck periodically — flow rate drifts with hopper fill level and
  granule size.
- **Sensor** — `readSensor()` currently returns randomized placeholder
  values. Replace it with real reads once your NPK sensor is wired in.

## Endpoints

| Route         | Method | Purpose                                      |
|---------------|--------|-----------------------------------------------|
| `/`           | GET    | Serves the dashboard                          |
| `/data`       | GET    | Preview reading (no dispense, no log)         |
| `/status`     | GET    | Device state, cooldown remaining, plant count |
| `/log`        | GET    | Full plant event log                          |
| `/dispense`   | POST   | Manual single-nutrient dispense (testing)     |

## License

MIT — see `LICENSE` (or replace with your preferred license).
