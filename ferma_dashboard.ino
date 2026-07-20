/*
  FERMA Soil Dashboard — ESP32 web server + per-plant servo dispensing
  -----------------------------------------------------------------------
  Workflow this sketch supports:
   - Device is hand-carried to a plant.
   - Operator presses the physical button (wired to BUTTON_PIN).
   - Device reads the sensor, computes a per-plant N/P/K dose, then opens
     each gate ONE AT A TIME (N, then P, then K) for a calculated duration.
   - The event (reading + grams dispensed) is logged in memory with an
     incrementing plant number.
   - Device enters a cooldown period so a stray press (e.g. while walking
     to the next plant) can't trigger a second dispense too soon.
   - The web dashboard polls /status and /log over WiFi to show live
     device state and pull in each new plant record automatically —
     no browser interaction is required to actually dispense.

  Libraries needed (Arduino IDE > Tools > Manage Libraries):
   - "ESP32Servo" by Kevin Harrington / madhephaestus
   - "ArduinoJson" by Benoit Blanchon (v6 or v7)

  Setup steps:
   1. Install the "ESP32" board package in Arduino IDE (Boards Manager).
   2. Install the "LittleFS" filesystem uploader plugin for Arduino IDE, OR
      use `arduino-cli` / PlatformIO's LittleFS upload feature.
   3. Keep index.html inside the /data folder next to this .ino file
      (already arranged that way in this download):
          ferma_dashboard/
            ferma_dashboard.ino
            data/
              index.html
   4. Use "ESP32 Sketch Data Upload" (or equivalent) to flash index.html
      onto the board's LittleFS partition.
   5. Wire:
        - Push button between BUTTON_PIN and GND (uses internal pull-up,
          so no external resistor needed).
        - Three gate servos on SERVO_PIN_N / _P / _K.
   6. Fill in your WiFi SSID/password below, upload, then open the Serial
      Monitor at 115200 baud to see the IP address.

  Replacing fake data with a real sensor later:
   - Edit readSensor() to read your actual NPK sensor instead of
     generating random numbers.

  CALIBRATING DOSE AND FLOW RATE (do this before trusting amounts):
   - RECO_MODEL below (target index + maxGrams per plant) must match the
     RECO_MODEL in index.html — they're kept in sync manually, so if you
     change one, change the other.
   - FLOW_RATE_G_PER_SEC per nutrient is a placeholder guess. To calibrate:
     open a gate for a fixed time (e.g. 5 seconds) over an empty container,
     weigh what came out, then set
       FLOW_RATE_G_PER_SEC = grams_collected / seconds_open
     Flow rate can drift with hopper fill level and granule size, so
     recheck periodically.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

const char* WIFI_SSID     = "YOUR_ROUTER_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_ROUTER_WIFI_PASSWORD";

WebServer server(80);

// ---- Button ---------------------------------------------------------------
const int BUTTON_PIN = 4; // button between this pin and GND
bool lastButtonReading = HIGH;
unsigned long lastDebounceMs = 0;
const unsigned long DEBOUNCE_MS = 50;

// ---- Servo gates ------------------------------------------------------------
const int SERVO_PIN_N = 13;
const int SERVO_PIN_P = 14;
const int SERVO_PIN_K = 27;
const int GATE_CLOSED_ANGLE = 0;
const int GATE_OPEN_ANGLE   = 90;
Servo servoN, servoP, servoK;

// PLACEHOLDER calibration — replace with your measured values (see header).
float FLOW_RATE_G_PER_SEC_N = 5.0;
float FLOW_RATE_G_PER_SEC_P = 5.0;
float FLOW_RATE_G_PER_SEC_K = 5.0;
const float MAX_DISPENSE_SECONDS = 15.0; // safety cap per nutrient per plant

// ---- Per-plant dose model — MUST match RECO_MODEL in index.html -----------
struct NutrientModel { const char* key; int target; float maxGrams; };
NutrientModel RECO_MODEL[3] = {
  { "n", 70, 10.0 },
  { "p", 75, 8.0  },
  { "k", 93, 8.0  },
};

float doseGrams(int reading, const NutrientModel& m) {
  float grams = m.maxGrams * (float)(m.target - reading) / (float)m.target;
  return grams > 0 ? grams : 0;
}

// ---- Device state machine --------------------------------------------------
enum DeviceState { STATE_READY, STATE_DISPENSING, STATE_COOLDOWN };
DeviceState deviceState = STATE_READY;
unsigned long cooldownUntilMs = 0;
const unsigned long COOLDOWN_MS = 8000; // time after finishing a plant before the next press is accepted

// ---- In-memory plant log ----------------------------------------------------
struct PlantEvent {
  int plant;
  unsigned long ts_ms;
  int n, p, k;
  float n_g, p_g, k_g;
};
const int MAX_LOG = 200;
PlantEvent logBuf[MAX_LOG];
int logCount = 0;      // how many slots are filled (caps at MAX_LOG)
int logStart = 0;      // circular buffer start index once full
int plantCounter = 0;

void addLogEntry(const PlantEvent& ev) {
  int idx = (logStart + logCount) % MAX_LOG;
  if (logCount < MAX_LOG) {
    logBuf[idx] = ev;
    logCount++;
  } else {
    logBuf[logStart] = ev;
    logStart = (logStart + 1) % MAX_LOG;
  }
}

// ---- Sensor read (replace with real sensor later) --------------------------
void readSensor(int &n, int &p, int &k) {
  n = random(40, 90);
  p = random(50, 80);
  k = random(80, 100);
}

// ---- Dispense one gate for a computed duration -----------------------------
float dispenseGate(Servo &gate, float grams, float flowRate) {
  if (grams <= 0 || flowRate <= 0) return 0;
  float durationSec = grams / flowRate;
  if (durationSec > MAX_DISPENSE_SECONDS) durationSec = MAX_DISPENSE_SECONDS;
  gate.write(GATE_OPEN_ANGLE);
  delay((unsigned long)(durationSec * 1000));
  gate.write(GATE_CLOSED_ANGLE);
  delay(300); // brief pause between gates so dispenses are clearly one-at-a-time
  return durationSec;
}

// ---- Full per-plant cycle: read, dose, dispense N->P->K in sequence, log ---
void runPlantCycle() {
  deviceState = STATE_DISPENSING;

  int n, p, k;
  readSensor(n, p, k);
  int readings[3] = { n, p, k };

  float grams[3];
  for (int i = 0; i < 3; i++) grams[i] = doseGrams(readings[i], RECO_MODEL[i]);

  Serial.printf("Plant #%d reading N=%d P=%d K=%d\n", plantCounter + 1, n, p, k);

  float g_n = dispenseGate(servoN, grams[0], FLOW_RATE_G_PER_SEC_N);
  float g_p = dispenseGate(servoP, grams[1], FLOW_RATE_G_PER_SEC_P);
  float g_k = dispenseGate(servoK, grams[2], FLOW_RATE_G_PER_SEC_K);
  (void)g_n; (void)g_p; (void)g_k; // durations available if you want to log them too

  plantCounter++;
  PlantEvent ev = { plantCounter, millis(), n, p, k, grams[0], grams[1], grams[2] };
  addLogEntry(ev);

  deviceState = STATE_COOLDOWN;
  cooldownUntilMs = millis() + COOLDOWN_MS;
}

// ---- Button polling (call from loop) ---------------------------------------
void pollButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastDebounceMs = millis();
  }
  if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
    static bool stablePressed = false;
    bool pressed = (reading == LOW);
    if (pressed && !stablePressed) {
      // Rising edge of a debounced press.
      if (deviceState == STATE_READY) {
        runPlantCycle();
      } else {
        Serial.println("Button pressed but device not ready (dispensing or cooling down) — ignored.");
      }
    }
    stablePressed = pressed;
  }
  lastButtonReading = reading;
}

// ---- HTTP handlers ----------------------------------------------------------

void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain", "index.html not found in LittleFS");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleData() {
  // Preview-only reading, used by the dashboard's "Preview Reading" button.
  // Does NOT dispense and does NOT log a plant record.
  int n, p, k;
  readSensor(n, p, k);
  String json = "{\"n\":" + String(n) + ",\"p\":" + String(p) + ",\"k\":" + String(k) + "}";
  server.send(200, "application/json", json);
}

// Manual/remote dispense of a single nutrient, independent of the button
// workflow. Uses the same per-plant dose model; useful for testing a gate
// without walking to a plant.
void handleDispense() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"POST only\"}");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  String key = doc["nutrient"] | "";
  float grams = doc["grams"] | 0.0;
  key.toLowerCase();

  Servo* gate = nullptr;
  float flowRate = 0;
  if (key == "n") { gate = &servoN; flowRate = FLOW_RATE_G_PER_SEC_N; }
  else if (key == "p") { gate = &servoP; flowRate = FLOW_RATE_G_PER_SEC_P; }
  else if (key == "k") { gate = &servoK; flowRate = FLOW_RATE_G_PER_SEC_K; }

  if (!gate || grams <= 0) {
    server.send(400, "application/json", "{\"error\":\"unknown nutrient or bad amount\"}");
    return;
  }

  deviceState = STATE_DISPENSING;
  float durationSec = dispenseGate(*gate, grams, flowRate);
  deviceState = STATE_COOLDOWN;
  cooldownUntilMs = millis() + COOLDOWN_MS;

  String resp = "{\"status\":\"ok\",\"nutrient\":\"" + key + "\",\"grams\":" + String(grams, 1) +
                ",\"duration_s\":" + String(durationSec, 2) + "}";
  server.send(200, "application/json", resp);
}

void handleStatus() {
  const char* stateStr = deviceState == STATE_READY ? "ready"
    : deviceState == STATE_DISPENSING ? "dispensing" : "cooldown";
  long remaining = 0;
  if (deviceState == STATE_COOLDOWN) {
    long ms = (long)cooldownUntilMs - (long)millis();
    remaining = ms > 0 ? (ms + 999) / 1000 : 0;
  }
  String json = "{\"state\":\"" + String(stateStr) + "\",\"cooldown_remaining_s\":" + String(remaining) +
                ",\"plant_count\":" + String(plantCounter) + "}";
  server.send(200, "application/json", json);
}

void handleLog() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < logCount; i++) {
    int idx = (logStart + i) % MAX_LOG;
    PlantEvent &ev = logBuf[idx];
    JsonObject o = arr.add<JsonObject>();
    o["plant"] = ev.plant;
    o["ts_ms"] = ev.ts_ms;
    o["n"] = ev.n; o["p"] = ev.p; o["k"] = ev.k;
    o["n_g"] = serialized(String(ev.n_g, 1));
    o["p_g"] = serialized(String(ev.p_g, 1));
    o["k_g"] = serialized(String(ev.k_g, 1));
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — did you upload the /data folder?");
  }

  servoN.attach(SERVO_PIN_N);
  servoP.attach(SERVO_PIN_P);
  servoK.attach(SERVO_PIN_K);
  servoN.write(GATE_CLOSED_ANGLE);
  servoP.write(GATE_CLOSED_ANGLE);
  servoK.write(GATE_CLOSED_ANGLE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! Open this in a browser on the same WiFi: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/dispense", HTTP_POST, handleDispense);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/log", HTTP_GET, handleLog);
  server.begin();
}

void loop() {
  server.handleClient();

  // Cooldown expiry.
  if (deviceState == STATE_COOLDOWN && millis() >= cooldownUntilMs) {
    deviceState = STATE_READY;
  }

  pollButton();
}
