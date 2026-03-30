// ============================================================
//  SafeStep Night System — ESP32-CAM Integrated Bridge
//
//  Combines:
//   1) K66F UART JSON bridge
//   2) WiFi caregiver dashboard
//   3) CSV logging in SPIFFS
//   4) ESP32-CAM capture + upload
//
//  BOARD: Freenove ESP32-WROVER + external OV2640
//
//  UART WIRING:
//    K66F TX  ->  ESP32 RX (GPIO13)
//    K66F RX  ->  ESP32 TX (GPIO12)
//    K66F GND ->  ESP32 GND
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_log.h"

// ============================================================
// Camera pin map for Freenove ESP32-WROVER + external OV2640
// ============================================================
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   21
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     19
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     5
#define Y2_GPIO_NUM     4
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ============================================================
// SECTION 1 — CONFIGURATION
// ============================================================
const char* WIFI_SSID     = "xx";
const char* WIFI_PASSWORD = "xx";

// Upload API / backend gallery
const char* serverUrl     = "http://xx:5000/api/upload-raw";
const char* galleryUrl    = "http://xx.180:5000/gallery";
const char* escalationUrl = "http://xx.180:5000/api/escalate";

// UART with K66F
HardwareSerial K66Serial(2);
static const int K66_RX_PIN = 13;   // ESP32 RX <- K66F TX
static const int K66_TX_PIN = 12;   // ESP32 TX -> K66F RX
static const long UART_BAUD = 115200;

// Alert mapping from K66F
static const int ALERT_NONE            = 0;
static const int ALERT_UNUSUAL_MOTION  = 1;
static const int ALERT_FALL            = 2;

// CSV Log
const char* CSV_PATH     = "/log.csv";
const int   CSV_MAX_ROWS = 500;
const char* CSV_HEADER   = "time,system,status,ldr,dark,pir,led,pwm,alert,bed_exit\n";

// LDR threshold UI range
static const uint16_t LDR_THRESHOLD_MIN     = 0;
static const uint16_t LDR_THRESHOLD_MAX     = 4095;
static const uint16_t LDR_THRESHOLD_DEFAULT = 2200;

// ============================================================
// SECTION 2 — GLOBALS
// ============================================================
WebServer server(80);

String latestJson =
  "{\"system\":\"off\",\"status\":\"off\",\"time\":\"--:--:--\","
  "\"ldr\":0,\"dark\":0,\"pir\":0,\"led\":0,\"pwm\":0,\"pressure_raw\":0,"
  "\"bed_exit\":0,\"alert\":0}";

String latestDiagJson =
  "{\"diag\":1,"
  "\"overall_fault\":0,"
  "\"dht22\":\"off\","
  "\"rtc\":\"off\","
  "\"ldr\":\"off\","
  "\"pir\":\"off\","
  "\"pressure\":\"off\","
  "\"esp32_link\":\"off\","
  "\"pwm_led\":\"off\","
  "\"buzzer\":\"off\"}";

String latestImageUrl = "";

SemaphoreHandle_t dataMutex;
static bool cameraReady = false;
static bool fsReady = false;

// Latest threshold mirrored from K66F config reply
static uint16_t currentLdrThreshold = LDR_THRESHOLD_DEFAULT;

// Escalation state
static bool escalationActive = false;
static bool escalationEmailSent = false;
static String escalationEvent = "";
static String escalationTime = "";
static String escalationMessage = "";

// UART freshness tracking for dashboard disconnect detection
static unsigned long lastUartRxTime = 0;
static unsigned long lastDiagRxTime = 0;
static const unsigned long UART_TIMEOUT_MS = 3000;

// ============================================================
// SECTION 3 — CSV LOGGER
// ============================================================
int csvCountRows() {
  if (!fsReady) return 0;

  File f = SPIFFS.open(CSV_PATH, "r");
  if (!f) return 0;

  int rows = 0;
  bool firstLine = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');

    if (firstLine) {
      firstLine = false;
      continue;
    }

    line.trim();
    if (line.length() > 0) rows++;
  }

  f.close();
  return rows;
}

void csvDropOldestRow() {
  if (!fsReady) return;

  File src = SPIFFS.open(CSV_PATH, "r");
  if (!src) return;

  String contents = src.readString();
  src.close();

  int headerEnd = contents.indexOf('\n');
  if (headerEnd == -1) return;

  String header = contents.substring(0, headerEnd + 1);

  int firstRowEnd = contents.indexOf('\n', headerEnd + 1);
  if (firstRowEnd == -1) return;

  String trimmed = header + contents.substring(firstRowEnd + 1);

  File dst = SPIFFS.open(CSV_PATH, "w");
  if (!dst) return;
  dst.print(trimmed);
  dst.close();

  Serial.println("[CSV] Oldest row dropped (rolling buffer)");
}

void csvAppendRow(const String& time, const String& system, const String& status,
                  int ldr, int dark, int pir, int led, int pwm, int alert, int bedExit) {
  if (!fsReady) {
    Serial.println("[CSV] Skipped: filesystem not ready");
    return;
  }

  if (!SPIFFS.exists(CSV_PATH)) {
    File f = SPIFFS.open(CSV_PATH, "w");
    if (f) {
      f.print(CSV_HEADER);
      f.close();
      Serial.println("[CSV] Created new log file");
    } else {
      Serial.println("[CSV] ERROR: could not create log file");
      return;
    }
  }

  int rows = csvCountRows();
  if (rows >= CSV_MAX_ROWS) {
    csvDropOldestRow();
    rows = csvCountRows();
  }

  File f = SPIFFS.open(CSV_PATH, "a");
  if (!f) {
    Serial.println("[CSV] ERROR: could not open file for append");
    return;
  }

  char row[192];
  snprintf(row, sizeof(row), "%s,%s,%s,%d,%d,%d,%d,%d,%d,%d\n",
           time.c_str(), system.c_str(), status.c_str(),
           ldr, dark, pir, led, pwm, alert, bedExit);

  f.print(row);
  f.close();

  Serial.printf("[CSV] Row appended (%d/%d): %s", rows + 1, CSV_MAX_ROWS, row);
}

void csvLogIfChanged(const String& time, const String& system, const String& status,
                     int ldr, int dark, int pir, int led, int pwm, int alert, int bedExit,
                     const String& prevSystem, int& prevPir, int& prevLed,
                     int& prevAlert, int& prevBedExit) {
  bool systemChanged = (system != prevSystem);
  bool ledChanged    = (led != prevLed);
  bool pirRising     = (pir && !prevPir);
  bool alertChanged  = (alert != prevAlert);
  bool bedExitPulse  = (bedExit && !prevBedExit);

  if (systemChanged || ledChanged || pirRising || alertChanged || bedExitPulse) {
    csvAppendRow(time, system, status, ldr, dark, pir, led, pwm, alert, bedExit);
  }
}

// ============================================================
// SECTION 4 — CAMERA FUNCTIONS
// ============================================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_VGA);
  }

  Serial.println("[CAM] Camera ready");
  return true;
}

bool uploadFrame(camera_fb_t* fb) {
  if (!fb) return false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CAM] Upload skipped: WiFi not connected");
    return false;
  }

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/octet-stream");

  Serial.printf("[CAM] Uploading %u bytes...\n", fb->len);
  int httpCode = http.POST(fb->buf, fb->len);

  bool ok = false;

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[CAM] HTTP %d\n", httpCode);
    Serial.println("[CAM] Server: " + response);

    if (httpCode == 200) {
      ok = true;

      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, response);

      if (!err) {
        if (doc["data"]["fullUrl"].is<const char*>()) {
          latestImageUrl = String((const char*)doc["data"]["fullUrl"]);
        } else if (doc["data"]["imageUrl"].is<const char*>()) {
          latestImageUrl = String((const char*)doc["data"]["imageUrl"]);
        }
      }
    }
  } else {
    Serial.printf("[CAM] HTTP POST failed: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
  return ok;
}

void captureAndUpload() {
  if (!cameraReady) {
    Serial.println("[CAM] Camera not ready");
    K66Serial.println("CAM_NOT_READY");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAM] Camera capture failed");
    K66Serial.println("CAPTURE_FAIL");
    return;
  }

  Serial.printf("[CAM] Captured image size: %u bytes\n", fb->len);

  bool ok = uploadFrame(fb);
  esp_camera_fb_return(fb);

  if (ok) {
    Serial.println("[CAM] Upload success");
    K66Serial.println("CAPTURE_OK");
  } else {
    Serial.println("[CAM] Upload failed");
    K66Serial.println("UPLOAD_FAIL");
  }
}

bool sendEscalationToBackend(const String& event) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESC] Cannot send escalation: WiFi not connected");
    escalationActive = true;
    escalationEmailSent = false;
    escalationEvent = event;
    escalationTime = "offline";
    escalationMessage = "WiFi disconnected";
    return false;
  }

  HTTPClient http;
  http.begin(escalationUrl);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> reqDoc;
  reqDoc["event"] = event;
  reqDoc["source"] = "esp32-cam";
  reqDoc["deviceIp"] = WiFi.localIP().toString();

  String requestBody;
  serializeJson(reqDoc, requestBody);

  Serial.println("[ESC] Sending escalation to backend: " + requestBody);

  int httpCode = http.POST(requestBody);
  String response = http.getString();

  escalationActive = true;
  escalationEvent = event;
  escalationTime = String(millis());

  if (httpCode > 0) {
    Serial.printf("[ESC] Backend HTTP %d\n", httpCode);
    Serial.println("[ESC] Response: " + response);

    if (httpCode == 200) {
      escalationEmailSent = true;
      escalationMessage = "Email sent";
      http.end();
      return true;
    } else {
      escalationEmailSent = false;
      escalationMessage = "Backend error";
      http.end();
      return false;
    }
  } else {
    Serial.printf("[ESC] POST failed: %s\n", http.errorToString(httpCode).c_str());
    escalationEmailSent = false;
    escalationMessage = "HTTP POST failed";
    http.end();
    return false;
  }
}

// ============================================================
// SECTION 5 — JSON MERGE HELPERS
// ============================================================
void mergeJsonIntoLatest(const JsonDocument& patchDoc) {
  StaticJsonDocument<1024> baseDoc;

  xSemaphoreTake(dataMutex, portMAX_DELAY);

  DeserializationError baseErr = deserializeJson(baseDoc, latestJson);
  if (baseErr) {
    baseDoc.clear();
    baseDoc["system"] = "off";
    baseDoc["status"] = "off";
    baseDoc["time"] = "--:--:--";
    baseDoc["ldr"] = 0;
    baseDoc["dark"] = 0;
    baseDoc["pir"] = 0;
    baseDoc["led"] = 0;
    baseDoc["pwm"] = 0;
    baseDoc["pressure_raw"] = 0;
    baseDoc["bed_exit"] = 0;
    baseDoc["alert"] = 0;
  }

  if (!patchDoc["system"].isNull())       baseDoc["system"] = patchDoc["system"];
  if (!patchDoc["status"].isNull())       baseDoc["status"] = patchDoc["status"];
  if (!patchDoc["time"].isNull())         baseDoc["time"] = patchDoc["time"];
  if (!patchDoc["ldr"].isNull())          baseDoc["ldr"] = patchDoc["ldr"];
  if (!patchDoc["dark"].isNull())         baseDoc["dark"] = patchDoc["dark"];
  if (!patchDoc["pir"].isNull())          baseDoc["pir"] = patchDoc["pir"];
  if (!patchDoc["led"].isNull())          baseDoc["led"] = patchDoc["led"];
  if (!patchDoc["pwm"].isNull())          baseDoc["pwm"] = patchDoc["pwm"];
  if (!patchDoc["pressure_raw"].isNull()) baseDoc["pressure_raw"] = patchDoc["pressure_raw"];
  if (!patchDoc["bed_exit"].isNull())     baseDoc["bed_exit"] = patchDoc["bed_exit"];
  if (!patchDoc["alert"].isNull())        baseDoc["alert"] = patchDoc["alert"];

  String merged;
  serializeJson(baseDoc, merged);
  latestJson = merged;

  xSemaphoreGive(dataMutex);
}

// ============================================================
// SECTION 6 — UART READER
// ============================================================
bool readCommandLine(String& out) {
  static String rxBuffer = "";

  while (K66Serial.available()) {
    char c = (char)K66Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      out = rxBuffer;
      rxBuffer = "";
      out.trim();
      return out.length() > 0;
    }

    if (rxBuffer.length() < 512) {
      rxBuffer += c;
    } else {
      rxBuffer = "";
    }
  }

  return false;
}

bool isCommandJson(const JsonDocument& doc) {
  return doc["cmd"].is<const char*>() || doc["cmd"].is<String>();
}

bool isThresholdJson(const JsonDocument& doc) {
  return doc["ldr_threshold"].is<uint32_t>() ||
         doc["ldr_threshold"].is<int>() ||
         doc["ldr_threshold"].is<const char*>() ||
         doc["ldr_threshold"].is<String>();
}

bool isDebugJson(const JsonDocument& doc) {
  return doc["uart_debug"].is<const char*>() || doc["uart_debug"].is<String>();
}

bool isDiagJson(const JsonDocument& doc) {
  return (doc["diag"].is<int>() || doc["diag"].is<bool>() || doc["overall_fault"].is<int>()) &&
         (doc["dht22"].is<const char*>() || doc["dht22"].is<String>() ||
          doc["rtc"].is<const char*>() || doc["rtc"].is<String>() ||
          doc["ldr"].is<const char*>() || doc["ldr"].is<String>() ||
          doc["pir"].is<const char*>() || doc["pir"].is<String>() ||
          doc["pressure"].is<const char*>() || doc["pressure"].is<String>() ||
          doc["esp32_link"].is<const char*>() || doc["esp32_link"].is<String>() ||
          doc["pwm_led"].is<const char*>() || doc["pwm_led"].is<String>() ||
          doc["buzzer"].is<const char*>() || doc["buzzer"].is<String>());
}

bool isFullSensorJson(const JsonDocument& doc) {
  return doc["system"].is<const char*>() ||
         doc["status"].is<const char*>() ||
         doc["time"].is<const char*>() ||
         doc["ldr"].is<int>() ||
         doc["dark"].is<int>() ||
         doc["pir"].is<int>() ||
         doc["led"].is<int>() ||
         doc["pwm"].is<int>() ||
         doc["pressure_raw"].is<int>() ||
         doc["bed_exit"].is<int>() ||
         doc["alert"].is<int>();
}

void processDiagJson(const String& line, JsonDocument& doc) {
  (void)doc;
  lastDiagRxTime = millis();

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  latestDiagJson = line;
  xSemaphoreGive(dataMutex);

  Serial.println("[UART] Health JSON: " + line);
}

void processThresholdJson(JsonDocument& doc) {
  if (doc["ldr_threshold"].is<uint32_t>() || doc["ldr_threshold"].is<int>()) {
    int val = doc["ldr_threshold"].as<int>();
    if (val < LDR_THRESHOLD_MIN) val = LDR_THRESHOLD_MIN;
    if (val > LDR_THRESHOLD_MAX) val = LDR_THRESHOLD_MAX;
    currentLdrThreshold = (uint16_t)val;
    Serial.printf("[UART] LDR threshold updated from K66F: %u\n", currentLdrThreshold);
  } else {
    const char* txt = doc["ldr_threshold"] | "";
    if (strcmp(txt, "default") == 0) {
      currentLdrThreshold = LDR_THRESHOLD_DEFAULT;
      Serial.printf("[UART] LDR threshold reset to default: %u\n", currentLdrThreshold);
    }
  }
}

void processDebugJson(JsonDocument& doc) {
  String msg = doc["uart_debug"] | "";

  if (msg.equals("RX:DIAG_LINK") || msg.equals("CMD_MATCH:DIAG_LINK")) {
    return;
  }

  Serial.println("[UARTDBG] " + msg);
}

void processSensorJson(const String& line, JsonDocument& doc,
                       String& prevSystem, int& prevPir, int& prevLed,
                       int& prevAlert, int& prevBedExit) {
  lastUartRxTime = millis();

  mergeJsonIntoLatest(doc);
  Serial.println("[UART] K66F JSON: " + line);

  int ldr       = doc["ldr"]      | 0;
  int dark      = doc["dark"]     | 0;
  int pir       = doc["pir"]      | 0;
  int led       = doc["led"]      | 0;
  int pwm       = doc["pwm"]      | 0;
  int alert     = doc["alert"]    | 0;
  int bedExit   = doc["bed_exit"] | 0;
  String t      = doc["time"]     | String("--");
  String system = doc["system"]   | String("off");
  String status = doc["status"]   | String("off");

  csvLogIfChanged(t, system, status, ldr, dark, pir, led, pwm, alert, bedExit,
                  prevSystem, prevPir, prevLed, prevAlert, prevBedExit);

  prevSystem  = system;
  prevPir     = pir;
  prevLed     = led;
  prevAlert   = alert;
  prevBedExit = bedExit;

  if (alert == ALERT_NONE) {
    escalationActive = false;
    escalationEmailSent = false;
    escalationEvent = "";
    escalationTime = "";
    escalationMessage = "";
  }
}

void processCommandJson(JsonDocument& doc) {
  String cmd   = doc["cmd"]   | "";
  String event = doc["event"] | "";

  cmd.trim();
  event.trim();

  if (cmd.equalsIgnoreCase("capture")) {
    Serial.printf("[CAPTURE] Capture command received FROM K66F (event=%s) -> executing\n", event.c_str());
    captureAndUpload();
    return;
  }

  if (cmd.equalsIgnoreCase("escalate")) {
    Serial.printf("[UART] Escalation command received (event=%s)\n", event.c_str());

    bool ok = sendEscalationToBackend(event.length() ? event : "fall");

    if (ok) {
      Serial.println("[ESC] Escalation email sent successfully");
    } else {
      Serial.println("[ESC] Escalation email failed");
    }
    return;
  }

  Serial.printf("[UART] Unknown JSON command: cmd=%s event=%s\n", cmd.c_str(), event.c_str());
}

void uartReadTask(void* param) {
  (void)param;

  String prevSystem = "";
  int prevPir = 0, prevLed = 0, prevAlert = 0, prevBedExit = 0;

  while (true) {
    String line;
    if (readCommandLine(line)) {
      lastUartRxTime = millis();

      if (line.startsWith("{")) {
        StaticJsonDocument<384> doc;
        DeserializationError err = deserializeJson(doc, line);

        if (err == DeserializationError::Ok) {
          if (isCommandJson(doc)) {
            processCommandJson(doc);
          } else if (isThresholdJson(doc)) {
            processThresholdJson(doc);
          } else if (isDebugJson(doc)) {
            processDebugJson(doc);
          } else if (isDiagJson(doc)) {
            processDiagJson(line, doc);
          } else if (isFullSensorJson(doc)) {
            processSensorJson(line, doc, prevSystem, prevPir, prevLed, prevAlert, prevBedExit);
          } else {
            Serial.println("[UART] Unclassified JSON: " + line);
          }
        } else {
          Serial.println("[UART] JSON parse error: " + String(err.c_str()));
          Serial.println("[UART] Raw line: " + line);
        }
      }
      else if (line.equalsIgnoreCase("CAPTURE")) {
        Serial.println("[UART] Plain CAPTURE command received from K66F");
        captureAndUpload();
      }
      else if (line.equalsIgnoreCase("PING")) {
        K66Serial.println("PONG");
      }
      else {
        Serial.println("[UART] Non-JSON / unknown line: " + line);
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ============================================================
// SECTION 7 — WIFI
// ============================================================
void connectWiFi() {
  Serial.printf("[WiFi] Starting connection to SSID: %s\n", WIFI_SSID);

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("[WiFi] Connect failed, status=");
    Serial.println(WiFi.status());
  }
}

// ============================================================
// SECTION 8 — HTTP HANDLERS
// ============================================================
void handleData() {
  String payload;
  String diagPayload;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  payload = latestJson;
  diagPayload = latestDiagJson;
  xSemaphoreGive(dataMutex);

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", payload);
    return;
  }

  const unsigned long now = millis();
  const bool uartConnected = (lastUartRxTime != 0 && (now - lastUartRxTime) <= UART_TIMEOUT_MS);
  const bool diagFresh = (lastDiagRxTime != 0 && (now - lastDiagRxTime) <= UART_TIMEOUT_MS);

  doc["uart_connected"] = uartConnected ? 1 : 0;
  doc["uart_timeout_ms"] = UART_TIMEOUT_MS;
  doc["escalated"] = escalationActive ? 1 : 0;
  doc["escalation_email_sent"] = escalationEmailSent ? 1 : 0;
  doc["escalation_event"] = escalationEvent;
  doc["escalation_time"] = escalationTime;
  doc["escalation_note"] = escalationMessage;
  doc["ldr_threshold"] = currentLdrThreshold;

  StaticJsonDocument<512> diagDoc;
  DeserializationError diagErr = deserializeJson(diagDoc, diagPayload);

  if (!diagErr && diagFresh) {
    doc["diag"] = diagDoc["diag"] | 1;
    doc["overall_fault"] = diagDoc["overall_fault"] | 0;

    doc["dht22"] = diagDoc["dht22"] | "warning";
    doc["rtc"] = diagDoc["rtc"] | "warning";
    doc["ldr_health"] = diagDoc["ldr"] | "warning";
    doc["pir_health"] = diagDoc["pir"] | "warning";
    doc["pressure_health"] = diagDoc["pressure"] | "warning";
    doc["esp32_link"] = diagDoc["esp32_link"] | "warning";
    doc["pwm_led_health"] = diagDoc["pwm_led"] | "warning";
    doc["buzzer_health"] = diagDoc["buzzer"] | "warning";

    if (!diagDoc["ldr_raw"].isNull()) {
      doc["diag_ldr_raw"] = diagDoc["ldr_raw"];
    }
    if (!diagDoc["pressure_raw"].isNull()) {
      doc["diag_pressure_raw"] = diagDoc["pressure_raw"];
    }
    if (!diagDoc["temp_x10"].isNull()) {
      doc["diag_temp_x10"] = diagDoc["temp_x10"];
    }
    if (!diagDoc["hum_x10"].isNull()) {
      doc["diag_hum_x10"] = diagDoc["hum_x10"];
    }
  } else {
    doc["diag"] = 0;
    doc["overall_fault"] = 0;
    doc["dht22"] = "off";
    doc["rtc"] = "off";
    doc["ldr_health"] = "off";
    doc["pir_health"] = "off";
    doc["pressure_health"] = "off";
    doc["esp32_link"] = uartConnected ? "warning" : "fault";
    doc["pwm_led_health"] = "off";
    doc["buzzer_health"] = "off";
  }

  if (!uartConnected) {
    doc["system"] = "disconnected";
    doc["status"] = "fault";
    doc["time"] = "--:--:--";
    doc["ldr"] = 0;
    doc["dark"] = 0;
    doc["pir"] = 0;
    doc["led"] = 0;
    doc["pwm"] = 0;
    doc["pressure_raw"] = 0;
    doc["bed_exit"] = 0;
    doc["alert"] = 0;
    doc["esp32_link"] = "fault";
  }

  String out;
  serializeJson(doc, out);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleLatestImage() {
  String json = "{\"url\":\"" + latestImageUrl + "\"}";
  server.send(200, "application/json", json);
}

void handleImagesRedirect() {
  String dashboardUrl = "http://" + WiFi.localIP().toString() + "/";
  String redirectUrl = String(galleryUrl) + "?back=" + dashboardUrl;

  server.sendHeader("Location", redirectUrl, true);
  server.send(302, "text/plain", "Redirecting to gallery...");
}

void handleDownloadLog() {
  if (!fsReady) {
    server.send(500, "text/plain", "Filesystem not mounted.");
    return;
  }

  if (!SPIFFS.exists(CSV_PATH)) {
    server.send(404, "text/plain", "No log file yet.");
    return;
  }

  File f = SPIFFS.open(CSV_PATH, "r");
  if (!f) {
    server.send(500, "text/plain", "Could not open log.");
    return;
  }

  server.sendHeader("Content-Disposition",
                    "attachment; filename=\"safestep_log.csv\"");
  server.streamFile(f, "text/csv");
  f.close();

  Serial.println("[CSV] Log downloaded via browser");
}

unsigned long lastDiagTx = 0;
const unsigned long DIAG_TX_PERIOD_MS = 2000;

void sendDiagHeartbeatToK66F() {
  unsigned long now = millis();

  if (now - lastDiagTx < DIAG_TX_PERIOD_MS) return;

  lastDiagTx = now;
  K66Serial.print("DIAG_LINK\n");
  K66Serial.flush();
}

void handleLogStats() {
  if (!fsReady) {
    server.send(500, "application/json", "{\"error\":\"filesystem not mounted\"}");
    return;
  }

  int rows = csvCountRows();
  size_t used = SPIFFS.usedBytes();
  size_t total = SPIFFS.totalBytes();

  String json = "{\"rows\":" + String(rows) +
                ",\"max\":" + String(CSV_MAX_ROWS) +
                ",\"used_bytes\":" + String(used) +
                ",\"total_bytes\":" + String(total) + "}";

  server.send(200, "application/json", json);
}

void handleClearLog() {
  if (!fsReady) {
    server.send(500, "application/json", "{\"error\":\"filesystem not mounted\"}");
    return;
  }

  if (SPIFFS.exists(CSV_PATH)) {
    SPIFFS.remove(CSV_PATH);
  }

  Serial.println("[CSV] Log cleared via dashboard");
  server.send(200, "application/json", "{\"cleared\":true}");
}

void handleCaptureNow() {
  Serial.println("[CAPTURE] Manual capture requested -> sent to K66F");
  K66Serial.print("MANUAL_CAPTURE\n");
  K66Serial.flush();
  server.send(200, "application/json", "{\"capture\":\"requested_from_k66f\"}");
}

void handleCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"error\":\"missing cmd\"}");
    return;
  }

  String cmd = server.arg("cmd");
  cmd.trim();

  if (cmd.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"empty cmd\"}");
    return;
  }

  if (cmd.equalsIgnoreCase("CAPTURE")) {
    Serial.println("[DASH->K66F] Manual capture requested");
    K66Serial.print("MANUAL_CAPTURE\n");
    K66Serial.flush();
    server.send(200, "application/json", "{\"sent\":\"MANUAL_CAPTURE\"}");
    return;
  }

  if (cmd.equalsIgnoreCase("ACK")) {
    Serial.println("[DASH->K66F] ACK requested");
    escalationActive = false;
    escalationEmailSent = false;
    escalationEvent = "";
    escalationTime = "";
    escalationMessage = "Acknowledged";
  }
  else if (cmd.startsWith("ldr_setting_change:")) {
    String v = cmd.substring(strlen("ldr_setting_change:"));
    Serial.println("[DASH->K66F] LDR threshold change -> " + v);
  }
  else if (cmd.equals("ldr_setting_set_default")) {
    Serial.println("[DASH->K66F] LDR threshold reset -> default");
  }
  else if (cmd.equals("SYS_ON")) {
    Serial.println("[DASH->K66F] System ON requested");
  }
  else if (cmd.equals("SYS_OFF")) {
    Serial.println("[DASH->K66F] System OFF requested");
  }
  else if (cmd.equals("PAUSE")) {
    Serial.println("[DASH->K66F] Pause/Resume requested");
  }
  else if (cmd.equals("STATUS")) {
    Serial.println("[DASH->K66F] Status refresh requested");
  }
  else {
    Serial.println("[DASH->K66F] Command -> " + cmd);
  }

  String txLine = cmd + "\n";
  K66Serial.print(txLine);
  K66Serial.flush();

  server.send(200, "application/json", "{\"sent\":\"" + cmd + "\"}");
}

void handleRoot() {
  extern const char DASHBOARD_HTML[];
  server.send(200, "text/html", DASHBOARD_HTML);
}

// ============================================================
// SECTION 9 — DASHBOARD HTML
// ============================================================
const char DASHBOARD_HTML[] = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SafeStep — Caregiver Dashboard</title>
  <link href="https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=Syne:wght@400;600;800&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg:#0a0e17;
      --surface:#111827;
      --border:#1e2d40;
      --accent:#38bdf8;
      --green:#34d399;
      --amber:#fbbf24;
      --red:#f87171;
      --muted:#4b5563;
      --text:#e2e8f0;
      --subtext:#64748b;
      --sidebar:#0f172a;
    }
    * { box-sizing:border-box; margin:0; padding:0; }
    body {
      background:var(--bg);
      color:var(--text);
      font-family:'Syne',sans-serif;
      min-height:100vh;
    }
    .app {
      display:grid;
      grid-template-columns:220px 1fr;
      min-height:100vh;
    }
    .sidebar {
      background:var(--sidebar);
      border-right:1px solid var(--border);
      padding:24px 16px;
      display:flex;
      flex-direction:column;
      gap:14px;
    }
    .sidebar h2 {
      color:var(--accent);
      font-size:1.1rem;
      margin-bottom:6px;
    }
    .sidebar p {
      color:var(--subtext);
      font-size:.8rem;
      font-family:'DM Mono',monospace;
      margin-bottom:10px;
    }
    .side-link {
      display:block;
      text-decoration:none;
      background:var(--surface);
      border:1px solid var(--border);
      color:var(--text);
      padding:12px 14px;
      border-radius:10px;
      font-size:.9rem;
      transition:.2s;
    }
    .side-link:hover {
      border-color:var(--accent);
      color:var(--accent);
    }
    .main {
      padding:32px 24px;
    }
    .header {
      display:flex;
      align-items:flex-start;
      justify-content:space-between;
      flex-wrap:wrap;
      gap:16px;
      margin-bottom:32px;
      padding-bottom:24px;
      border-bottom:1px solid var(--border);
    }
    .header-left h1 {
      font-size:1.8em;
      font-weight:800;
      color:var(--accent);
    }
    .header-left p {
      color:var(--subtext);
      font-size:.85em;
      margin-top:4px;
      font-family:'DM Mono',monospace;
    }
    .conn-badge {
      display:flex;
      align-items:center;
      gap:8px;
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:999px;
      padding:8px 16px;
      font-size:.8em;
      font-family:'DM Mono',monospace;
    }
    .conn-dot {
      width:8px; height:8px;
      border-radius:50%;
      background:var(--muted);
      transition:background .4s;
    }
    .conn-dot.live { background:var(--green); box-shadow:0 0 6px var(--green); }
    .conn-dot.err  { background:var(--red);   box-shadow:0 0 6px var(--red); }
    .conn-dot.off  { background:var(--amber); box-shadow:0 0 6px var(--amber); }

    #motionAlertBanner, #pausedBanner, #offBanner, #disconnectedBanner, #shutdownBanner, #bedExitBanner, #escalationBanner {
      display:none;
      align-items:center;
      gap:12px;
      border-radius:12px;
      padding:14px 20px;
      margin-bottom:16px;
    }
    #motionAlertBanner {
      background:rgba(251,191,36,.12);
      border:1px solid var(--amber);
    }
    #pausedBanner {
      background:rgba(56,189,248,.08);
      border:1px solid var(--accent);
    }
    #offBanner {
      background:rgba(248,113,113,.08);
      border:1px solid var(--red);
    }
    #disconnectedBanner {
      background:rgba(248,113,113,.12);
      border:1px solid var(--red);
    }
    #shutdownBanner {
      background:rgba(251,191,36,.10);
      border:1px solid var(--amber);
    }
    #bedExitBanner {
      background:rgba(251,191,36,.10);
      border:1px solid var(--amber);
    }
    #escalationBanner {
      background:rgba(248,113,113,.10);
      border:1px solid var(--red);
    }

    #fallOverlay {
      display:none;
      position:fixed;
      inset:0;
      background:rgba(10,14,23,.94);
      z-index:9999;
      align-items:center;
      justify-content:center;
      padding:24px;
    }
    #fallOverlay.visible {
      display:flex;
    }
    .fall-modal {
      width:min(720px, 100%);
      background:#190f12;
      border:2px solid var(--red);
      border-radius:20px;
      padding:28px 24px;
      text-align:center;
      box-shadow:0 0 28px rgba(248,113,113,.25);
      animation:fallPulse 1.1s infinite;
    }
    @keyframes fallPulse {
      0%,100% { box-shadow:0 0 18px rgba(248,113,113,.20); }
      50% { box-shadow:0 0 34px rgba(248,113,113,.45); }
    }
    .fall-icon {
      font-size:3rem;
      margin-bottom:10px;
    }
    .fall-title {
      font-size:1.6rem;
      font-weight:800;
      color:var(--red);
      margin-bottom:10px;
    }
    .fall-subtitle {
      color:var(--text);
      font-size:.95rem;
      margin-bottom:18px;
    }
    .fall-actions {
      display:flex;
      gap:12px;
      justify-content:center;
      flex-wrap:wrap;
      margin-bottom:14px;
    }
    .fall-note {
      font-family:'DM Mono',monospace;
      color:var(--subtext);
      font-size:.8rem;
    }

    .alert-icon { font-size:1.4em; }
    .alert-text { flex:1; font-weight:600; font-size:.95em; }
    .alert-text span {
      color:var(--subtext);
      font-size:.85em;
      font-weight:400;
      margin-left:8px;
    }

    .status-strip {
      display:flex;
      flex-wrap:wrap;
      gap:12px;
      margin-bottom:28px;
    }
    .status-pill {
      display:flex;
      align-items:center;
      gap:10px;
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:12px;
      padding:14px 20px;
      flex:1;
      min-width:140px;
    }
    .status-pill.active-green { border-color:var(--green); }
    .status-pill.active-amber { border-color:var(--amber); }
    .status-pill.active-red   { border-color:var(--red); }
    .status-pill.active-blue  { border-color:var(--accent); }

    .pill-label {
      font-size:.7em;
      color:var(--subtext);
      text-transform:uppercase;
      letter-spacing:1px;
    }
    .pill-value {
      font-size:.95em;
      font-weight:600;
      color:var(--text);
      margin-top:2px;
    }

    .section-title {
      font-size:.7em;
      text-transform:uppercase;
      letter-spacing:2px;
      color:var(--subtext);
      margin-bottom:14px;
      font-family:'DM Mono',monospace;
    }

    .cards-grid {
      display:grid;
      grid-template-columns:repeat(auto-fill,minmax(150px,1fr));
      gap:12px;
      margin-bottom:28px;
    }
    .card {
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:12px;
      padding:20px 16px;
      text-align:center;
      transition:opacity .2s;
    }
    .card.muted { opacity:.55; }
    .card .c-value {
      font-size:2em;
      font-weight:800;
      color:var(--accent);
      font-family:'DM Mono',monospace;
    }
    .card .c-unit {
      font-size:.75em;
      color:var(--subtext);
      margin-left:2px;
    }
    .card .c-label {
      font-size:.7em;
      color:var(--subtext);
      text-transform:uppercase;
      letter-spacing:1px;
      margin-top:6px;
    }

    .content-grid {
      display:grid;
      grid-template-columns:minmax(0, 1fr) 320px;
      gap:16px;
      align-items:start;
    }
    .left-stack { min-width:0; }
    .right-stack {
      display:flex;
      flex-direction:column;
      gap:16px;
      position:sticky;
      top:20px;
    }

    .bottom-grid {
      display:grid;
      grid-template-columns:1fr 1fr;
      gap:16px;
    }
    @media (max-width:1100px) {
      .content-grid { grid-template-columns:1fr; }
      .right-stack { position:static; }
    }
    @media (max-width:900px) {
      .app { grid-template-columns:1fr; }
      .sidebar {
        border-right:none;
        border-bottom:1px solid var(--border);
      }
    }
    @media (max-width:640px) {
      .bottom-grid { grid-template-columns:1fr; }
    }

    .panel {
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:14px;
      padding:20px;
    }
    .cmd-input-row {
      display:flex;
      gap:8px;
      margin-bottom:12px;
    }
    input[type=text],
    input[type=number] {
      flex:1;
      background:var(--bg);
      border:1px solid var(--border);
      border-radius:8px;
      padding:10px 14px;
      color:var(--text);
      font-family:'DM Mono',monospace;
      font-size:.85em;
    }
    input[type=number] {
      flex:0 0 110px;
      min-width:110px;
    }
    .btn {
      border:none;
      border-radius:8px;
      padding:10px 16px;
      font-family:'Syne',sans-serif;
      font-weight:600;
      font-size:.82em;
      cursor:pointer;
    }
    .btn:disabled {
      opacity:.45;
      cursor:not-allowed;
    }
    .btn-blue { background:var(--accent); color:#0a0e17; }
    .btn-green { background:var(--green); color:#0a0e17; }
    .btn-red { background:var(--red); color:#fff; }
    .btn-ghost { background:var(--border); color:var(--text); }
    .quick-btns { display:flex; flex-wrap:wrap; gap:8px; }

    .threshold-wrap {
      margin-top:14px;
      padding-top:14px;
      border-top:1px solid var(--border);
    }
    .threshold-row {
      display:flex;
      align-items:center;
      gap:10px;
      flex-wrap:wrap;
    }
    .threshold-slider-wrap {
      flex:1 1 220px;
      min-width:220px;
    }
    #ldrThresholdSlider {
      width:100%;
      accent-color: var(--accent);
    }
    .threshold-meta {
      display:flex;
      justify-content:space-between;
      gap:10px;
      margin-top:8px;
      font-family:'DM Mono',monospace;
      font-size:.74em;
      color:var(--subtext);
    }
    .threshold-live { color:var(--accent); }

    #log {
      font-family:'DM Mono',monospace;
      font-size:.75em;
      height:220px;
      overflow-y:auto;
      display:flex;
      flex-direction:column;
      gap:4px;
      margin-top:10px;
    }
    .log-entry {
      display:grid;
      grid-template-columns:auto auto 1fr;
      gap:10px;
      padding:5px 8px;
      border-radius:6px;
      background:var(--bg);
      align-items:center;
    }
    .log-time { color:var(--muted); white-space:nowrap; }
    .log-badge {
      font-size:.7em;
      padding:2px 7px;
      border-radius:4px;
      font-weight:600;
      text-transform:uppercase;
      white-space:nowrap;
    }
    .badge-cmd { background:rgba(56,189,248,.15); color:var(--accent); }
    .badge-alert { background:rgba(248,113,113,.15); color:var(--red); }
    .badge-info { background:rgba(100,116,139,.15); color:var(--subtext); }
    .badge-motion { background:rgba(251,191,36,.15); color:var(--amber); }
    .log-msg { color:var(--text); }

    .progress-wrap {
      width:100%;
      height:10px;
      border-radius:999px;
      background:var(--bg);
      border:1px solid var(--border);
      overflow:hidden;
    }
    .progress-bar {
      width:0%;
      height:100%;
      background:var(--accent);
      transition:width .25s ease;
    }

    .health-summary {
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      margin-bottom:14px;
      padding:12px 14px;
      border-radius:12px;
      border:1px solid var(--border);
      background:var(--bg);
    }
    .health-summary-title {
      font-size:.78rem;
      color:var(--subtext);
      text-transform:uppercase;
      letter-spacing:1px;
      font-family:'DM Mono',monospace;
    }
    .health-summary-value {
      font-size:.96rem;
      font-weight:700;
    }
    .health-summary.ok { border-color:rgba(52,211,153,.45); }
    .health-summary.warn { border-color:rgba(251,191,36,.45); }
    .health-summary.fault { border-color:rgba(248,113,113,.45); }

    .health-list {
      display:flex;
      flex-direction:column;
      gap:10px;
    }
    .health-row {
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      padding:12px 14px;
      border-radius:12px;
      border:1px solid var(--border);
      background:var(--bg);
    }
    .health-left {
      display:flex;
      align-items:center;
      gap:10px;
      min-width:0;
    }
    .health-icon {
      width:34px;
      height:34px;
      border-radius:10px;
      display:grid;
      place-items:center;
      background:var(--surface);
      border:1px solid var(--border);
      font-size:1rem;
      flex-shrink:0;
    }
    .health-text { min-width:0; }
    .health-name {
      font-size:.88rem;
      font-weight:700;
      color:var(--text);
    }
    .health-sub {
      font-size:.72rem;
      color:var(--subtext);
      font-family:'DM Mono',monospace;
      margin-top:2px;
    }
    .health-badge {
      border-radius:999px;
      padding:6px 10px;
      font-size:.72rem;
      font-weight:700;
      text-transform:uppercase;
      font-family:'DM Mono',monospace;
      border:1px solid var(--border);
      white-space:nowrap;
    }
    .health-badge.ok {
      color:var(--green);
      border-color:rgba(52,211,153,.45);
      background:rgba(52,211,153,.08);
    }
    .health-badge.warning {
      color:var(--amber);
      border-color:rgba(251,191,36,.45);
      background:rgba(251,191,36,.08);
    }
    .health-badge.fault {
      color:var(--red);
      border-color:rgba(248,113,113,.45);
      background:rgba(248,113,113,.08);
    }
    .health-badge.unknown {
      color:var(--subtext);
      background:rgba(100,116,139,.08);
    }
  </style>
</head>
<body>
  <div id="fallOverlay">
    <div class="fall-modal">
      <div class="fall-icon">🚨</div>
      <div class="fall-title">FALL ALERT ACTIVE</div>
      <div class="fall-subtitle">
        Brightness override is active. Only acknowledge or image view is allowed.
      </div>
      <div class="fall-actions">
        <button class="btn btn-red" onclick="sendCmd('ACK')">ACKNOWLEDGE ALERT</button>
        <button class="btn btn-blue" onclick="openLastImage()">VIEW LAST IMAGE</button>
      </div>
      <div id="lastImageStatus" class="fall-note">No uploaded image available yet.</div>
    </div>
  </div>

  <div class="app">
    <aside class="sidebar">
      <h2>SafeStep</h2>
      <p>Navigation</p>
      <a class="side-link" href="/">Dashboard</a>
      <a class="side-link" href="/images" target="_blank">Saved Images</a>
    </aside>

    <main class="main">
      <div class="header">
        <div class="header-left">
          <h1>SafeStep Night System</h1>
          <p>Caregiver Monitoring Dashboard</p>
        </div>
        <div class="conn-badge">
          <div class="conn-dot" id="connDot"></div>
          <span id="connText">Connecting...</span>
        </div>
      </div>

      <div class="content-grid">
        <div class="left-stack">
          <div id="offBanner">
            <div class="alert-icon">⏹️</div>
            <div class="alert-text">
              SYSTEM OFF
              <span>Press SW3 on the K66F board to turn the system on</span>
            </div>
          </div>

          <div id="disconnectedBanner">
            <div class="alert-icon">📡</div>
            <div class="alert-text">
              K66F DISCONNECTED
              <span>No recent UART data from the K66F board</span>
            </div>
          </div>

          <div id="pausedBanner">
            <div class="alert-icon">⏸️</div>
            <div class="alert-text">
              SYSTEM PAUSED
              <span>Use PAUSE again or SW2 to resume, or SYS_OFF to turn it off</span>
            </div>
          </div>

          <div id="shutdownBanner">
            <div class="alert-icon">⚠️</div>
            <div class="alert-text">
              SYSTEM SHUTDOWN PENDING
              <span>System will turn off shortly</span>
            </div>
          </div>

          <div id="bedExitBanner">
            <div class="alert-icon">🛏️</div>
            <div class="alert-text">
              BED EXIT ALERT
              <span>Patient left the bed</span>
            </div>
          </div>

          <div id="motionAlertBanner">
            <div class="alert-icon">⚠️</div>
            <div class="alert-text">
              UNUSUAL MOTION ALERT
              <span>Warning only. Monitoring continues.</span>
            </div>
            <button class="btn btn-red" onclick="sendCmd('ACK')">ACK ALERT</button>
          </div>

          <div id="escalationBanner">
            <div class="alert-icon">📧</div>
            <div class="alert-text">
              FALL ESCALATED
              <span>Caregiver email notification has been triggered</span>
            </div>
          </div>

          <div class="section-title">Live Status</div>
          <div class="status-strip">
            <div class="status-pill" id="pill-system">
              <div>⚡</div>
              <div><div class="pill-label">System</div><div class="pill-value" id="val-system">—</div></div>
            </div>
            <div class="status-pill" id="pill-env">
              <div id="icon-env">🌙</div>
              <div><div class="pill-label">Environment</div><div class="pill-value" id="val-env">—</div></div>
            </div>
            <div class="status-pill" id="pill-motion">
              <div>🚶</div>
              <div><div class="pill-label">Motion</div><div class="pill-value" id="val-motion">—</div></div>
            </div>
            <div class="status-pill" id="pill-led">
              <div>💡</div>
              <div><div class="pill-label">Pathway Light</div><div class="pill-value" id="val-led">—</div></div>
            </div>
            <div class="status-pill" id="pill-time">
              <div>🕐</div>
              <div><div class="pill-label">Last Update</div><div class="pill-value" id="val-time" style="font-size:.82em">—</div></div>
            </div>
          </div>

          <div class="section-title">Sensor Data</div>
          <div class="cards-grid">
            <div class="card" id="card-ldr">
              <div class="c-value" id="c-ldr">—</div>
              <div class="c-label">Light Level (LDR)</div>
            </div>
            <div class="card" id="card-pwm">
              <div class="c-value" id="c-pwm">—<span class="c-unit">%</span></div>
              <div class="c-label">LED Brightness</div>
            </div>
          </div>

          <div class="bottom-grid">
            <div class="panel">
              <div class="section-title">Send Command</div>
              <div class="cmd-input-row">
                <input type="text" id="cmdInput" class="dash-control" placeholder="Custom command..." />
                <button class="btn btn-blue dash-control" onclick="sendCmd()">Send</button>
              </div>
              <div class="quick-btns">
                <button class="btn btn-green dash-control" onclick="sendCmd('SYS_ON')">▶ System ON</button>
                <button class="btn btn-red dash-control" onclick="sendCmd('SYS_OFF')">■ System OFF</button>
                <button class="btn btn-ghost dash-control" onclick="sendCmd('PAUSE')">⏸ Pause</button>
                <button class="btn btn-ghost dash-control" onclick="sendCmd('ACK')">✓ ACK Alert</button>
                <button class="btn btn-ghost dash-control" onclick="sendCmd('STATUS')">↺ Status</button>
                <button class="btn btn-blue dash-control" onclick="sendCmd('CAPTURE')">📷 Capture Image</button>
              </div>

              <div class="threshold-wrap">
                <div class="section-title">LDR Threshold</div>
                <div class="threshold-row">
                  <div class="threshold-slider-wrap">
                    <input
                      id="ldrThresholdSlider"
                      class="dash-control"
                      type="range"
                      min="0"
                      max="4095"
                      value="2200"
                      oninput="syncThresholdFromSlider()"
                    />
                  </div>

                  <input
                    id="ldrThresholdNumber"
                    class="dash-control"
                    type="number"
                    min="0"
                    max="4095"
                    value="2200"
                    oninput="syncThresholdFromNumber()"
                  />

                  <button class="btn btn-blue dash-control" data-threshold-action="set" onclick="setLdrThreshold()">Set</button>
                  <button class="btn btn-ghost dash-control" data-threshold-action="default" onclick="resetLdrThreshold()">Default</button>
                </div>

                <div class="threshold-meta">
                  <span>Range: 0–4095</span>
                  <span class="threshold-live">Saved threshold: <span id="ldrThresholdLive">2200</span></span>
                </div>
              </div>
            </div>

            <div class="panel">
              <div class="section-title">Activity Log</div>
              <div id="log"></div>
            </div>
          </div>

          <div class="panel" style="margin-top:16px">
            <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:14px">
              <div class="section-title" style="margin:0">Flash Log Storage</div>
              <div style="display:flex; gap:8px; flex-wrap:wrap">
                <a href="/log.csv" download="safestep_log.csv"><button class="btn btn-blue">⬇ Download CSV</button></a>
                <button class="btn btn-ghost" onclick="refreshLogStats()">↺ Refresh</button>
                <button class="btn btn-red" onclick="clearLog()">🗑 Clear Log</button>
              </div>
            </div>

            <div style="margin-bottom:10px">
              <div style="display:flex; justify-content:space-between; font-family:'DM Mono',monospace; font-size:.75em; color:var(--subtext); margin-bottom:6px">
                <span id="rowCountText">— / 500 rows</span>
                <span id="storageText">— KB used</span>
              </div>
              <div class="progress-wrap">
                <div class="progress-bar" id="logBar"></div>
              </div>
            </div>
          </div>
        </div>

        <div class="right-stack">
          <div class="panel">
            <div class="section-title">Sensor Health</div>

            <div id="healthSummary" class="health-summary warn">
              <div>
                <div class="health-summary-title">Overall Diagnostic</div>
                <div class="health-summary-value" id="healthSummaryText">Checking…</div>
              </div>
              <div id="healthSummaryIcon" style="font-size:1.4rem">🩺</div>
            </div>

            <div class="health-list">
              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🌡️</div>
                  <div class="health-text">
                    <div class="health-name">DHT22</div>
                    <div class="health-sub">temperature / humidity</div>
                  </div>
                </div>
                <div id="health-dht22" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🕒</div>
                  <div class="health-text">
                    <div class="health-name">RTC</div>
                    <div class="health-sub">time source</div>
                  </div>
                </div>
                <div id="health-rtc" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🌗</div>
                  <div class="health-text">
                    <div class="health-name">LDR</div>
                    <div class="health-sub">light sensor</div>
                  </div>
                </div>
                <div id="health-ldr" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🚶</div>
                  <div class="health-text">
                    <div class="health-name">PIR</div>
                    <div class="health-sub">motion sensor</div>
                  </div>
                </div>
                <div id="health-pir" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🧍</div>
                  <div class="health-text">
                    <div class="health-name">Pressure</div>
                    <div class="health-sub">mat / FSR</div>
                  </div>
                </div>
                <div id="health-pressure" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">📡</div>
                  <div class="health-text">
                    <div class="health-name">ESP32 Link</div>
                    <div class="health-sub">UART heartbeat</div>
                  </div>
                </div>
                <div id="health-esp32" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">💡</div>
                  <div class="health-text">
                    <div class="health-name">PWM LED</div>
                    <div class="health-sub">light driver path</div>
                  </div>
                </div>
                <div id="health-pwm" class="health-badge unknown">unknown</div>
              </div>

              <div class="health-row">
                <div class="health-left">
                  <div class="health-icon">🔔</div>
                  <div class="health-text">
                    <div class="health-name">Buzzer</div>
                    <div class="health-sub">alert path</div>
                  </div>
                </div>
                <div id="health-buzzer" class="health-badge unknown">unknown</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </main>
  </div>

  <script>
    const ALERT_NONE = 0;
    const ALERT_UNUSUAL_MOTION = 1;
    const ALERT_FALL = 2;

    const LDR_MIN = 0;
    const LDR_MAX = 4095;
    const LDR_DEFAULT = 2200;

    let prev = {};
    let bedExitBannerUntil = 0;
    let latestImageUrl = "";
    let fallLockActive = false;

    let thresholdEditing = false;
    let thresholdSaving = false;
    let lastServerThreshold = LDR_DEFAULT;

    const connDot = document.getElementById('connDot');
    const connText = document.getElementById('connText');
    const logBox = document.getElementById('log');

    function logEntry(msg, type='info') {
      const t = new Date().toLocaleTimeString();
      const badgeClass = {
        cmd:'badge-cmd', alert:'badge-alert', motion:'badge-motion', info:'badge-info'
      }[type] || 'badge-info';
      const label = {
        cmd:'CMD', alert:'ALERT', motion:'MOTION', info:'INFO'
      }[type] || 'INFO';

      const entry = document.createElement('div');
      entry.className = 'log-entry';
      entry.innerHTML = `
        <span class="log-time">${t}</span>
        <span class="log-badge ${badgeClass}">${label}</span>
        <span class="log-msg">${msg}</span>`;
      logBox.appendChild(entry);
      logBox.scrollTop = logBox.scrollHeight;

      while (logBox.children.length > 100) {
        logBox.removeChild(logBox.firstChild);
      }
    }

    function clampThreshold(val) {
      let n = parseInt(val, 10);
      if (isNaN(n)) n = LDR_DEFAULT;
      if (n < LDR_MIN) n = LDR_MIN;
      if (n > LDR_MAX) n = LDR_MAX;
      return n;
    }

    function getThresholdSlider() { return document.getElementById('ldrThresholdSlider'); }
    function getThresholdNumber() { return document.getElementById('ldrThresholdNumber'); }
    function getThresholdLive() { return document.getElementById('ldrThresholdLive'); }
    function getThresholdButtons() { return Array.from(document.querySelectorAll('[data-threshold-action]')); }

    function setThresholdUi(value, updateSavedLabel = false) {
      const v = clampThreshold(value);
      const slider = getThresholdSlider();
      const number = getThresholdNumber();
      const live = getThresholdLive();

      if (slider) slider.value = v;
      if (number) number.value = v;
      if (updateSavedLabel && live) live.textContent = String(v);
    }

    function setSavedThresholdLabel(value) {
      const live = getThresholdLive();
      if (live) live.textContent = String(clampThreshold(value));
    }

    function setThresholdBusyState(isBusy) {
      thresholdSaving = isBusy;

      const slider = getThresholdSlider();
      const number = getThresholdNumber();

      if (slider) slider.disabled = isBusy || fallLockActive;
      if (number) number.disabled = isBusy || fallLockActive;

      getThresholdButtons().forEach((btn) => {
        btn.disabled = isBusy || fallLockActive;
      });
    }

    function beginThresholdEditing() {
      if (fallLockActive || thresholdSaving) return;
      thresholdEditing = true;
    }

    function endThresholdEditing() {
      thresholdEditing = false;
    }

    function syncThresholdFromSlider() {
      if (fallLockActive || thresholdSaving) return;
      beginThresholdEditing();
      const value = clampThreshold(getThresholdSlider().value);
      getThresholdSlider().value = value;
      getThresholdNumber().value = value;
    }

    function syncThresholdFromNumber() {
      if (fallLockActive || thresholdSaving) return;
      beginThresholdEditing();
      const value = clampThreshold(getThresholdNumber().value);
      getThresholdSlider().value = value;
      getThresholdNumber().value = value;
    }

    function applyThresholdFromServer(value) {
      const v = clampThreshold(value);
      lastServerThreshold = v;
      setSavedThresholdLabel(v);

      if (fallLockActive || thresholdSaving || thresholdEditing) {
        return;
      }

      setThresholdUi(v, false);
    }

    function initThresholdControls() {
      const slider = getThresholdSlider();
      const number = getThresholdNumber();

      if (!slider || !number) return;

      slider.addEventListener('pointerdown', beginThresholdEditing);
      slider.addEventListener('mousedown', beginThresholdEditing);
      slider.addEventListener('touchstart', beginThresholdEditing, { passive: true });
      slider.addEventListener('focus', beginThresholdEditing);
      slider.addEventListener('input', syncThresholdFromSlider);
      slider.addEventListener('change', () => { syncThresholdFromSlider(); });
      slider.addEventListener('pointerup', endThresholdEditing);
      slider.addEventListener('mouseup', endThresholdEditing);
      slider.addEventListener('touchend', endThresholdEditing, { passive: true });
      slider.addEventListener('blur', endThresholdEditing);

      number.addEventListener('focus', beginThresholdEditing);
      number.addEventListener('input', syncThresholdFromNumber);
      number.addEventListener('change', () => {
        const value = clampThreshold(number.value);
        number.value = value;
        slider.value = value;
      });
      number.addEventListener('blur', () => {
        const value = clampThreshold(number.value);
        number.value = value;
        slider.value = value;
        endThresholdEditing();
      });

      setThresholdUi(LDR_DEFAULT, true);
    }

    async function setLdrThreshold() {
      if (fallLockActive) {
        logEntry('Dashboard locked during FALL alert. Threshold change is disabled.', 'alert');
        return;
      }

      if (thresholdSaving) return;

      const value = clampThreshold(getThresholdNumber().value);
      setThresholdUi(value, false);
      setThresholdBusyState(true);

      try {
        const res = await fetch('/command', {
          method:'POST',
          headers:{ 'Content-Type':'application/x-www-form-urlencoded' },
          body:'cmd=' + encodeURIComponent('ldr_setting_change:' + value)
        });

        const json = await res.json();

        if (!res.ok) throw new Error(json.error || 'Failed to send threshold command');

        logEntry('Sent → ' + (json.sent || ('ldr_setting_change:' + value)), 'cmd');
        endThresholdEditing();
        lastServerThreshold = value;
        setSavedThresholdLabel(value);
      } catch (e) {
        logEntry('Failed to set LDR threshold', 'alert');
        setThresholdUi(lastServerThreshold, false);
        setSavedThresholdLabel(lastServerThreshold);
      } finally {
        setThresholdBusyState(false);
      }
    }

    async function resetLdrThreshold() {
      if (fallLockActive) {
        logEntry('Dashboard locked during FALL alert. Threshold reset is disabled.', 'alert');
        return;
      }

      if (thresholdSaving) return;

      setThresholdBusyState(true);

      try {
        const res = await fetch('/command', {
          method:'POST',
          headers:{ 'Content-Type':'application/x-www-form-urlencoded' },
          body:'cmd=' + encodeURIComponent('ldr_setting_set_default')
        });

        const json = await res.json();

        if (!res.ok) throw new Error(json.error || 'Failed to reset threshold');

        logEntry('Sent → ' + (json.sent || 'ldr_setting_set_default'), 'cmd');
        endThresholdEditing();
        lastServerThreshold = LDR_DEFAULT;
        setThresholdUi(LDR_DEFAULT, false);
        setSavedThresholdLabel(LDR_DEFAULT);
      } catch (e) {
        logEntry('Failed to reset LDR threshold', 'alert');
        setThresholdUi(lastServerThreshold, false);
        setSavedThresholdLabel(lastServerThreshold);
      } finally {
        setThresholdBusyState(false);
      }
    }

    function setPill(id, activeClass, valueText) {
      const pill = document.getElementById('pill-' + id);
      pill.className = 'status-pill ' + (activeClass || '');
      document.getElementById('val-' + id).textContent = valueText;
    }

    function setSensorCardsMuted(muted) {
      const ldrCard = document.getElementById('card-ldr');
      const pwmCard = document.getElementById('card-pwm');
      if (ldrCard) ldrCard.classList.toggle('muted', muted);
      if (pwmCard) pwmCard.classList.toggle('muted', muted);
    }

    function showOnlyBanner(name) {
      document.getElementById('offBanner').style.display = 'none';
      document.getElementById('disconnectedBanner').style.display = 'none';
      document.getElementById('pausedBanner').style.display = 'none';
      document.getElementById('shutdownBanner').style.display = 'none';

      if (name === 'off') document.getElementById('offBanner').style.display = 'flex';
      if (name === 'disconnected') document.getElementById('disconnectedBanner').style.display = 'flex';
      if (name === 'paused') document.getElementById('pausedBanner').style.display = 'flex';
      if (name === 'shutdown') document.getElementById('shutdownBanner').style.display = 'flex';
    }

    function setDashboardLocked(locked) {
      fallLockActive = locked;

      document.querySelectorAll('.dash-control').forEach(el => {
        el.disabled = locked;
      });

      const cmdInput = document.getElementById('cmdInput');
      if (locked && cmdInput) cmdInput.blur();

      if (locked) {
        endThresholdEditing();
      }

      setThresholdBusyState(thresholdSaving);
    }

    async function refreshLatestImageUrl() {
      try {
        const res = await fetch('/image/latest');
        const d = await res.json();
        latestImageUrl = d.url || "";

        const note = document.getElementById('lastImageStatus');
        if (latestImageUrl) {
          note.textContent = 'Latest uploaded image is ready.';
        } else {
          note.textContent = 'No uploaded image available yet.';
        }
      } catch (e) {
        latestImageUrl = "";
        document.getElementById('lastImageStatus').textContent = 'Could not fetch image status.';
      }
    }

    function openLastImage() {
      if (!latestImageUrl) {
        document.getElementById('lastImageStatus').textContent = 'No uploaded image available yet.';
        return;
      }
      window.open(latestImageUrl, '_blank');
    }

    function updateAlertUi(alertCode) {
      const motionBanner = document.getElementById('motionAlertBanner');
      const fallOverlay = document.getElementById('fallOverlay');

      if (alertCode === ALERT_FALL) {
        motionBanner.style.display = 'none';
        fallOverlay.classList.add('visible');
        setDashboardLocked(true);
        refreshLatestImageUrl();
      } else {
        fallOverlay.classList.remove('visible');
        setDashboardLocked(false);

        if (alertCode === ALERT_UNUSUAL_MOTION) {
          motionBanner.style.display = 'flex';
        } else {
          motionBanner.style.display = 'none';
        }
      }
    }

    function updateBedExitBanner(bedExit) {
      const banner = document.getElementById('bedExitBanner');

      if (bedExit) {
        bedExitBannerUntil = Date.now() + 5000;
      }

      if (Date.now() < bedExitBannerUntil) {
        banner.style.display = 'flex';
      } else {
        banner.style.display = 'none';
      }
    }

    function normalizeHealthState(value) {
      const s = String(value || 'unknown').toLowerCase();
      if (s === 'ok') return 'ok';
      if (s === 'warning') return 'warning';
      if (s === 'fault') return 'fault';
      return 'unknown';
    }

    function setHealthBadge(id, state) {
      const el = document.getElementById(id);
      const normalized = normalizeHealthState(state);
      el.className = 'health-badge ' + normalized;
      el.textContent = normalized;
    }

    function updateHealthPanel(d) {
      const overallFault = Number(d.overall_fault || 0);

      setHealthBadge('health-dht22', d.dht22);
      setHealthBadge('health-rtc', d.rtc);
      setHealthBadge('health-ldr', d.ldr_health);
      setHealthBadge('health-pir', d.pir_health);
      setHealthBadge('health-pressure', d.pressure_health);
      setHealthBadge('health-esp32', d.esp32_link);
      setHealthBadge('health-pwm', d.pwm_led_health);
      setHealthBadge('health-buzzer', d.buzzer_health);

      const states = [
        normalizeHealthState(d.dht22),
        normalizeHealthState(d.rtc),
        normalizeHealthState(d.ldr_health),
        normalizeHealthState(d.pir_health),
        normalizeHealthState(d.pressure_health),
        normalizeHealthState(d.esp32_link),
        normalizeHealthState(d.pwm_led_health),
        normalizeHealthState(d.buzzer_health)
      ];

      const summary = document.getElementById('healthSummary');
      const text = document.getElementById('healthSummaryText');
      const icon = document.getElementById('healthSummaryIcon');

      summary.className = 'health-summary';

      if (overallFault || states.includes('fault')) {
        summary.classList.add('fault');
        text.textContent = 'Fault detected';
        icon.textContent = '🚨';
      } else if (states.includes('warning') || !Number(d.diag || 0)) {
        summary.classList.add('warn');
        text.textContent = 'Monitoring / warning';
        icon.textContent = '⚠️';
      } else {
        summary.classList.add('ok');
        text.textContent = 'All monitored paths healthy';
        icon.textContent = '✅';
      }
    }

    async function fetchData() {
      try {
        const res = await fetch('/data');
        const d = await res.json();

        const sys = String(d.system || '').toLowerCase();
        const alertCode = Number(d.alert || 0);
        const darkVal = Number(d.dark || 0);
        const pirVal = Number(d.pir || 0);
        const ledOn = Number(d.led || 0);
        const pwmVal = Number(d.pwm || 0);
        const bedExit = Number(d.bed_exit || 0);
        const escalated = Number(d.escalated || 0);
        const escalationEmailSent = Number(d.escalation_email_sent || 0);

        if (typeof d.ldr_threshold !== 'undefined') {
          applyThresholdFromServer(d.ldr_threshold);
        }

        updateHealthPanel(d);
        updateBedExitBanner(bedExit);

        const escalationBanner = document.getElementById('escalationBanner');
        if (escalated && escalationEmailSent) {
          escalationBanner.style.display = 'flex';
        } else {
          escalationBanner.style.display = 'none';
        }

        if (sys === 'disconnected') {
          connDot.className = 'conn-dot err';
          connText.textContent = 'K66F Disconnected';
          showOnlyBanner('disconnected');
          updateAlertUi(ALERT_NONE);

          setPill('system', 'active-red', 'Disconnected');
          setPill('env', '', 'No data');
          setPill('motion', '', 'No data');
          setPill('led', '', 'No data');
          setPill('time', '', '—');

          document.getElementById('icon-env').textContent = '📡';
          document.getElementById('c-ldr').textContent = '0';
          document.getElementById('c-pwm').innerHTML = `0<span class="c-unit">%</span>`;
          setSensorCardsMuted(true);

          if (prev.system !== 'disconnected') {
            logEntry('K66F UART disconnected', 'alert');
          }

          prev = { ...d };
          return;
        }

        if (sys === 'off') {
          connDot.className = 'conn-dot off';
          connText.textContent = 'System Off';
          showOnlyBanner('off');
          updateAlertUi(ALERT_NONE);

          setPill('system', 'active-red', 'Off');
          setPill('env', '', 'Inactive');
          setPill('motion', '', 'Inactive');
          setPill('led', '', 'Off');
          setPill('time', '', d.time || '—');

          document.getElementById('icon-env').textContent = '🌙';
          document.getElementById('c-ldr').textContent = '0';
          document.getElementById('c-pwm').innerHTML = `0<span class="c-unit">%</span>`;
          setSensorCardsMuted(true);

          if (prev.system && prev.system !== 'off') {
            logEntry('System turned OFF', 'info');
          }

          prev = { ...d };
          return;
        }

        if (sys === 'paused') {
          connDot.className = 'conn-dot live';
          connText.textContent = 'System Paused';
          showOnlyBanner('paused');
          updateAlertUi(ALERT_NONE);

          setPill('system', 'active-blue', 'Paused');
          setPill('env', '', 'Paused');
          setPill('motion', '', 'Paused');
          setPill('led', '', 'Off');
          setPill('time', '', d.time || '—');

          document.getElementById('icon-env').textContent = '⏸️';
          document.getElementById('c-ldr').textContent = (d.ldr ?? 0);
          document.getElementById('c-pwm').innerHTML = `0<span class="c-unit">%</span>`;

          setSensorCardsMuted(true);

          if (prev.system !== 'paused') {
            logEntry('System paused', 'info');
          }

          prev = { ...d };
          return;
        }

        if (sys === 'shutdown_pending') {
          connDot.className = 'conn-dot off';
          connText.textContent = 'Shutdown Pending';
          showOnlyBanner('shutdown');
          updateAlertUi(ALERT_NONE);

          setPill('system', 'active-amber', 'Stopping');
          setPill('env', '', 'Stopping');
          setPill('motion', '', 'Stopping');
          setPill('led', '', 'Stopping');
          setPill('time', '', d.time || '—');

          setSensorCardsMuted(true);

          if (prev.system !== 'shutdown_pending') {
            logEntry('System shutdown pending', 'info');
          }

          prev = { ...d };
          return;
        }

        connDot.className = 'conn-dot live';
        connText.textContent = 'Live';
        showOnlyBanner('');
        setSensorCardsMuted(false);

        if (prev.system && prev.system !== 'running') {
          logEntry('System running', 'info');
        }

        updateAlertUi(alertCode);
        setPill('system', 'active-green', 'Running');

        if (alertCode === ALERT_FALL && prev.alert !== ALERT_FALL) {
          logEntry('Fall alert triggered', 'alert');
        } else if (alertCode === ALERT_UNUSUAL_MOTION && prev.alert !== ALERT_UNUSUAL_MOTION) {
          logEntry('Unusual motion alert triggered', 'alert');
        } else if (alertCode === ALERT_NONE && Number(prev.alert || 0) != 0) {
          logEntry('Alert cleared / acknowledged', 'info');
        }

        if (escalated && !prev.escalated) {
          logEntry('Fall alert escalated — caregiver email sent', 'alert');
        }

        if (bedExit && !prev.bed_exit) {
          logEntry('Bed exit detected', 'motion');
        }

        if (darkVal) {
          document.getElementById('icon-env').textContent = '🌙';
          setPill('env', 'active-blue', 'Dark');
        } else {
          document.getElementById('icon-env').textContent = '☀️';
          setPill('env', '', 'Bright');
        }

        if (pirVal && !prev.pir) logEntry('Motion detected', 'motion');
        setPill('motion', pirVal ? 'active-amber' : '', pirVal ? 'Detected' : 'None');

        if (ledOn && !prev.led) logEntry('Pathway light turned ON', 'info');
        if (!ledOn && prev.led) logEntry('Pathway light turned OFF', 'info');

        if (alertCode === ALERT_FALL) {
          setPill('led', 'active-red', `OVERRIDE — ${pwmVal}%`);
        } else {
          setPill('led', ledOn ? 'active-green' : '', ledOn ? `ON — ${pwmVal}%` : 'OFF');
        }

        document.getElementById('val-time').textContent = d.time || '—';
        document.getElementById('c-ldr').textContent = (d.ldr ?? '—');
        document.getElementById('c-pwm').innerHTML = `${pwmVal}<span class="c-unit">%</span>`;
        prev = { ...d };
      } catch (e) {
        connDot.className = 'conn-dot err';
        connText.textContent = 'No connection';
      }
    }

    async function sendCmd(cmd) {
      const input = document.getElementById('cmdInput');
      const c = cmd || input.value.trim();
      if (!c) return;

      if (fallLockActive && c.toUpperCase() !== 'ACK') {
        logEntry('Dashboard locked during FALL alert. Only ACK and image view are allowed.', 'alert');
        input.value = '';
        return;
      }

      try {
        const res = await fetch('/command', {
          method:'POST',
          headers:{ 'Content-Type':'application/x-www-form-urlencoded' },
          body:'cmd=' + encodeURIComponent(c)
        });
        const json = await res.json();
        logEntry('Sent → ' + (json.sent || c), 'cmd');
      } catch (e) {
        logEntry('Failed to send command', 'alert');
      }

      input.value = '';
    }

    async function refreshLogStats() {
      try {
        const res = await fetch('/log/stats');
        const d = await res.json();

        const rows = Number(d.rows || 0);
        const max = Number(d.max || 500);
        const used = Number(d.used_bytes || 0);

        document.getElementById('rowCountText').textContent = `${rows} / ${max} rows`;
        document.getElementById('storageText').textContent = `${(used / 1024).toFixed(1)} KB used`;

        const pct = max > 0 ? Math.min((rows / max) * 100, 100) : 0;
        const bar = document.getElementById('logBar');
        bar.style.width = pct + '%';
        bar.style.background = pct >= 95 ? 'var(--red)' : pct >= 70 ? 'var(--amber)' : 'var(--accent)';
      } catch (e) {
        logEntry('Failed to fetch log stats', 'alert');
      }
    }

    async function clearLog() {
      try {
        const res = await fetch('/log/clear', { method:'POST' });
        if (!res.ok) throw new Error('clear failed');
        logEntry('Flash CSV log cleared', 'info');
        refreshLogStats();
      } catch (e) {
        logEntry('Failed to clear log', 'alert');
      }
    }

    window.addEventListener('load', () => {
      initThresholdControls();
    });

    setInterval(fetchData, 1000);
    fetchData();
    refreshLogStats();
    setInterval(refreshLogStats, 10000);
    logEntry('Dashboard connected to ESP32-CAM', 'info');
  </script>
</body>
</html>
)rawhtml";

// ============================================================
// SECTION 10 — SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  esp_log_level_set("*", ESP_LOG_NONE);
  delay(1000);

  Serial.println();
  Serial.println("=== SafeStep ESP32-CAM Integrated Bridge Booting ===");

  dataMutex = xSemaphoreCreateMutex();

  if (!SPIFFS.begin(true)) {
    fsReady = false;
    Serial.println("[FS] SPIFFS mount FAILED");
  } else {
    fsReady = true;
    Serial.printf("[FS] SPIFFS mounted — %d KB used / %d KB total\n",
                  SPIFFS.usedBytes() / 1024,
                  SPIFFS.totalBytes() / 1024);
    Serial.printf("[CSV] Log has %d rows (cap: %d)\n",
                  csvCountRows(), CSV_MAX_ROWS);
  }

  cameraReady = initCamera();

  Serial.println("[CONFIG] LIVE MODE — UART from K66F");
  K66Serial.begin(UART_BAUD, SERIAL_8N1, K66_RX_PIN, K66_TX_PIN);
  Serial.printf("[UART] K66F UART ready on RX=%d TX=%d\n", K66_RX_PIN, K66_TX_PIN);

  xTaskCreate(uartReadTask, "uartRead", 6144, NULL, 1, NULL);

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Dashboard → http://" + WiFi.localIP().toString());
  }

  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/data",         HTTP_GET,  handleData);
  server.on("/image/latest", HTTP_GET,  handleLatestImage);
  server.on("/images",       HTTP_GET,  handleImagesRedirect);
  server.on("/command",      HTTP_POST, handleCommand);
  server.on("/capture",      HTTP_GET,  handleCaptureNow);
  server.on("/capture",      HTTP_POST, handleCaptureNow);
  server.on("/log.csv",      HTTP_GET,  handleDownloadLog);
  server.on("/log/stats",    HTTP_GET,  handleLogStats);
  server.on("/log/clear",    HTTP_POST, handleClearLog);

  server.begin();

  Serial.println("[HTTP] Server started");
  Serial.println("====================================================");
  Serial.println();
}

// ============================================================
// SECTION 11 — MAIN LOOP
// ============================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  sendDiagHeartbeatToK66F();
  server.handleClient();
}