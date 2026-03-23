// ============================================================
//  SafeStep Night System — ESP32-CAM Integrated Bridge
//
//  Combines:
//   1) K66F UART JSON bridge
//   2) WiFi caregiver dashboard
//   3) CSV logging in SPIFFS
//   4) ESP32-CAM capture + upload
//
//  BOARD: AI Thinker ESP32-CAM
//
//  UART WIRING (current working setup):
//    K66F TX  ->  ESP32-CAM GPIO13 (RX)
//    K66F RX  ->  ESP32-CAM GPIO12 (TX)
//    K66F GND ->  ESP32-CAM GND
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include "esp32_camera.h"

// ===========================
// Camera model
// ===========================
#define CAMERA_MODEL_AI_THINKER
#include "pins.h"

// ============================================================
//  SECTION 1 — CONFIGURATION
// ============================================================

const char* WIFI_SSID     = "xxx";
const char* WIFI_PASSWORD = "xxx";

// Upload API / backend gallery
const char* serverUrl  = "http://xxxx:5000/api/upload-raw";
const char* galleryUrl = "http://xxxx:5000/gallery";

// UART with K66F
HardwareSerial K66Serial(2);
static const int K66_RX_PIN = 13;   // ESP32-CAM RX <- K66F TX
static const int K66_TX_PIN = 12;   // ESP32-CAM TX -> K66F RX
static const long UART_BAUD = 115200;

// Toggle this to test without K66F hardware
const bool TEST_MODE = false;

// Alert mapping from K66F
static const int ALERT_NONE            = 0;
static const int ALERT_UNUSUAL_MOTION  = 1;
static const int ALERT_FALL            = 2;

// -- CSV Log --
const char* CSV_PATH     = "/log.csv";
const int   CSV_MAX_ROWS = 500;
const char* CSV_HEADER   = "time,system,status,ldr,dark,pir,led,pwm,alert,count,bed_exit\n";

// ============================================================
//  SECTION 2 — GLOBALS
// ============================================================

WebServer server(80);

String latestJson =
  "{\"system\":\"off\",\"status\":\"off\",\"time\":\"--:--:--\","
  "\"ldr\":0,\"dark\":0,\"pir\":0,\"led\":0,\"pwm\":0,\"pressure_raw\":0,"
  "\"bed_exit\":0,\"alert\":0,\"count\":0}";

String latestImageUrl = "";

SemaphoreHandle_t dataMutex;
static bool cameraReady = false;
static bool fsReady = false;

// ============================================================
//  SECTION 3 — CSV LOGGER
// ============================================================

int csvCountRows() {
  if (!fsReady) return 0;

  File f = SPIFFS.open(CSV_PATH, "r");
  if (!f) return 0;

  int count = 0;
  bool firstLine = true;
  while (f.available()) {
    f.readStringUntil('\n');
    if (firstLine) {
      firstLine = false;
      continue;
    }
    count++;
  }
  f.close();
  return count;
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
                  int ldr, int dark, int pir, int led, int pwm, int alert, int count, int bedExit) {
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
  snprintf(row, sizeof(row), "%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d\n",
           time.c_str(), system.c_str(), status.c_str(),
           ldr, dark, pir, led, pwm, alert, count, bedExit);
  f.print(row);
  f.close();

  Serial.printf("[CSV] Row appended (%d/%d): %s", rows + 1, CSV_MAX_ROWS, row);
}

void csvLogIfChanged(const String& time, const String& system, const String& status,
                     int ldr, int dark, int pir, int led, int pwm, int alert, int count, int bedExit,
                     const String& prevSystem, int prevPir, int prevLed, int prevAlert, int prevBedExit) {
  bool systemChanged = (system != prevSystem);
  bool ledChanged    = (led != prevLed);
  bool pirRising     = (pir && !prevPir);
  bool alertChanged  = (alert != prevAlert);
  bool bedExitPulse  = (bedExit && !prevBedExit);

  if (systemChanged || ledChanged || pirRising || alertChanged || bedExitPulse) {
    csvAppendRow(time, system, status, ldr, dark, pir, led, pwm, alert, count, bedExit);
  }
}

// ============================================================
//  SECTION 4 — TEST MODE SIMULATION
// ============================================================

void testModeTask(void* param) {
  float ldr = 2800.0f;
  int pwm = 0;
  int count = 0;
  bool dark = true;
  bool pir = false;
  bool led = false;
  int alert = ALERT_NONE;
  int bedExit = 0;

  String systemState = "running";
  String statusState = "ok";

  String prevSystem = "";
  int prevPir = 0, prevLed = 0, prevAlert = 0, prevBedExit = 0;

  int hh = 23, mm = 0, ss = 0;

  while (true) {
    ldr += random(-50, 50);
    if (ldr < 100) ldr = 100;
    if (ldr > 4095) ldr = 4095;
    dark = (ldr > 2000);

    pir = (random(0, 10) < 2);

    // Normal motion accumulation -> unusual motion alert
    if (dark && pir) {
      led = true;
      pwm = map((int)ldr, 2000, 4095, 40, 100);

      if (alert == ALERT_NONE) {
        count++;
      }

      if (count >= 5 && alert == ALERT_NONE) {
        alert = ALERT_UNUSUAL_MOTION;
      }
    } else if (!pir && led) {
      if (random(0, 4) == 0) {
        led = false;
        pwm = 0;
      }
    }

    // Rare random fall simulation
    if (random(0, 60) == 0) {
      alert = ALERT_FALL;
      led = true;
      pwm = 100;
    }

    if (random(0, 25) == 0) {
      bedExit = 1;
    } else {
      bedExit = 0;
    }

    if (!dark) {
      led = false;
      pwm = 0;
      count = 0;
      if (alert == ALERT_UNUSUAL_MOTION) {
        alert = ALERT_NONE;
      }
    }

    ss++;
    if (ss >= 60) { ss = 0; mm++; }
    if (mm >= 60) { mm = 0; hh++; }
    if (hh >= 24) hh = 0;

    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", hh, mm, ss);

    StaticJsonDocument<320> doc;
    doc["ldr"]          = (int)ldr;
    doc["dark"]         = dark ? 1 : 0;
    doc["pir"]          = pir ? 1 : 0;
    doc["led"]          = led ? 1 : 0;
    doc["pwm"]          = pwm;
    doc["alert"]        = alert;
    doc["count"]        = count;
    doc["bed_exit"]     = bedExit;
    doc["pressure_raw"] = 0;
    doc["time"]         = timeBuf;
    doc["system"]       = systemState;
    doc["status"]       = statusState;

    String out;
    serializeJson(doc, out);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    latestJson = out;
    xSemaphoreGive(dataMutex);

    csvLogIfChanged(String(timeBuf), systemState, statusState,
                    (int)ldr, dark ? 1 : 0,
                    pir ? 1 : 0, led ? 1 : 0, pwm,
                    alert, count, bedExit,
                    prevSystem, prevPir, prevLed, prevAlert, prevBedExit);

    prevSystem  = systemState;
    prevPir     = pir ? 1 : 0;
    prevLed     = led ? 1 : 0;
    prevAlert   = alert;
    prevBedExit = bedExit;

    Serial.println("[TEST] " + out);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  SECTION 5 — CAMERA FUNCTIONS
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
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
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

// ============================================================
//  SECTION 6 — UART READER
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

void processSensorJson(const String& line, JsonDocument& doc,
                       String& prevSystem, int& prevPir, int& prevLed, int& prevAlert, int& prevBedExit) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  latestJson = line;
  xSemaphoreGive(dataMutex);

  Serial.println("[UART] K66F JSON: " + line);

  int ldr        = doc["ldr"]          | 0;
  int dark       = doc["dark"]         | 0;
  int pir        = doc["pir"]          | 0;
  int led        = doc["led"]          | 0;
  int pwm        = doc["pwm"]          | 0;
  int alert      = doc["alert"]        | 0;
  int count      = doc["count"]        | 0;
  int bedExit    = doc["bed_exit"]     | 0;
  String t       = doc["time"]         | String("--");
  String system  = doc["system"]       | String("off");
  String status  = doc["status"]       | String("off");

  csvLogIfChanged(t, system, status, ldr, dark, pir, led, pwm, alert, count, bedExit,
                  prevSystem, prevPir, prevLed, prevAlert, prevBedExit);

  prevSystem  = system;
  prevPir     = pir;
  prevLed     = led;
  prevAlert   = alert;
  prevBedExit = bedExit;
}

void processCommandJson(JsonDocument& doc) {
  String cmd   = doc["cmd"]   | "";
  String event = doc["event"] | "";

  cmd.trim();
  event.trim();

  if (cmd.equalsIgnoreCase("capture")) {
    Serial.printf("[UART] JSON capture command received (event=%s)\n", event.c_str());
    captureAndUpload();
    return;
  }

  Serial.printf("[UART] Unknown JSON command: cmd=%s event=%s\n", cmd.c_str(), event.c_str());
}

void uartReadTask(void* param) {
  String prevSystem = "";
  int prevPir = 0, prevLed = 0, prevAlert = 0, prevBedExit = 0;

  while (true) {
    String line;
    if (readCommandLine(line)) {
      if (line.startsWith("{")) {
        StaticJsonDocument<384> doc;
        DeserializationError err = deserializeJson(doc, line);

        if (err == DeserializationError::Ok) {
          if (isCommandJson(doc)) {
            processCommandJson(doc);
          } else {
            processSensorJson(line, doc, prevSystem, prevPir, prevLed, prevAlert, prevBedExit);
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
//  SECTION 7 — WIFI
// ============================================================

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
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
    return true;
  }

  Serial.println("[WiFi] Connect failed");
  return false;
}

// ============================================================
//  SECTION 8 — HTTP HANDLERS
// ============================================================

void handleData() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  String payload = latestJson;
  xSemaphoreGive(dataMutex);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", payload);
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
  captureAndUpload();
  server.send(200, "application/json", "{\"capture\":\"started\"}");
}

void handleCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"error\":\"missing cmd\"}");
    return;
  }

  String cmd = server.arg("cmd");
  cmd.trim();

  if (cmd.equalsIgnoreCase("CAPTURE")) {
    Serial.println("[HTTP] Local CAPTURE command");
    captureAndUpload();
    server.send(200, "application/json", "{\"sent\":\"CAPTURE\"}");
    return;
  }

  if (TEST_MODE) {
    Serial.println("[TEST] Command (not forwarded in test mode): " + cmd);
  } else {
    K66Serial.print(cmd + "\n");
    Serial.println("[UART] Sent to K66F: " + cmd);
  }

  server.send(200, "application/json", "{\"sent\":\"" + cmd + "\"}");
}

void handleRoot() {
  extern const char DASHBOARD_HTML[];
  server.send(200, "text/html", DASHBOARD_HTML);
}

// ============================================================
//  SECTION 9 — DASHBOARD HTML
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

    #motionAlertBanner, #pausedBanner, #offBanner, #shutdownBanner, #bedExitBanner {
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
    #shutdownBanner {
      background:rgba(251,191,36,.10);
      border:1px solid var(--amber);
    }
    #bedExitBanner {
      background:rgba(251,191,36,.10);
      border:1px solid var(--amber);
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

    .count-card { grid-column:span 2; }
    .progress-wrap {
      background:var(--border);
      border-radius:999px;
      height:8px;
      margin-top:10px;
      overflow:hidden;
    }
    .progress-bar {
      height:100%;
      border-radius:999px;
      background:var(--accent);
      transition:width .5s ease, background .3s;
    }
    .count-row {
      display:flex;
      justify-content:space-between;
      align-items:baseline;
      gap:8px;
    }

    .bottom-grid {
      display:grid;
      grid-template-columns:1fr 1fr;
      gap:16px;
    }
    @media (max-width:900px) {
      .app {
        grid-template-columns:1fr;
      }
      .sidebar {
        border-right:none;
        border-bottom:1px solid var(--border);
      }
    }
    @media (max-width:640px) {
      .bottom-grid { grid-template-columns:1fr; }
      .count-card { grid-column:span 1; }
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
    input[type=text] {
      flex:1;
      background:var(--bg);
      border:1px solid var(--border);
      border-radius:8px;
      padding:10px 14px;
      color:var(--text);
      font-family:'DM Mono',monospace;
      font-size:.85em;
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

      <div id="offBanner">
        <div class="alert-icon">⏹️</div>
        <div class="alert-text">
          SYSTEM OFF
          <span>Press SW3 on the K66F board to turn the system on</span>
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
        <div class="card count-card" id="card-count">
          <div class="count-row">
            <div>
              <div class="c-value" style="text-align:left" id="c-count">—<span class="c-unit" style="font-size:.5em"> / 5</span></div>
              <div class="c-label" style="text-align:left">Motion Events (Alert Threshold)</div>
            </div>
          </div>
          <div class="progress-wrap">
            <div class="progress-bar" id="countBar" style="width:0%"></div>
          </div>
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
            <div class="progress-bar" id="logBar" style="width:0%"></div>
          </div>
        </div>
      </div>
    </main>
  </div>

  <script>
    const ALERT_NONE = 0;
    const ALERT_UNUSUAL_MOTION = 1;
    const ALERT_FALL = 2;

    let prev = {};
    let bedExitBannerUntil = 0;
    let latestImageUrl = "";
    let fallLockActive = false;

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

    function setPill(id, activeClass, valueText) {
      const pill = document.getElementById('pill-' + id);
      pill.className = 'status-pill ' + (activeClass || '');
      document.getElementById('val-' + id).textContent = valueText;
    }

    function setSensorCardsMuted(muted) {
      document.getElementById('card-ldr').classList.toggle('muted', muted);
      document.getElementById('card-pwm').classList.toggle('muted', muted);
      document.getElementById('card-count').classList.toggle('muted', muted);
    }

    function showOnlyBanner(name) {
      document.getElementById('offBanner').style.display = 'none';
      document.getElementById('pausedBanner').style.display = 'none';
      document.getElementById('shutdownBanner').style.display = 'none';

      if (name === 'off') document.getElementById('offBanner').style.display = 'flex';
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
        const count = Number(d.count || 0);

        updateBedExitBanner(bedExit);

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
          document.getElementById('c-count').innerHTML = `0<span class="c-unit" style="font-size:.5em"> / 5</span>`;
          document.getElementById('countBar').style.width = '0%';

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
          document.getElementById('c-count').innerHTML = `${count}<span class="c-unit" style="font-size:.5em"> / 5</span>`;
          document.getElementById('countBar').style.width = Math.min((count / 5) * 100, 100) + '%';

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
        } else if (alertCode === ALERT_NONE && Number(prev.alert || 0) !== 0) {
          logEntry('Alert cleared / acknowledged', 'info');
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
        document.getElementById('c-count').innerHTML = `${count}<span class="c-unit" style="font-size:.5em"> / 5</span>`;

        const pct = Math.min((count / 5) * 100, 100);
        const bar = document.getElementById('countBar');
        bar.style.width = pct + '%';
        bar.style.background = pct >= 100 ? 'var(--red)' : pct >= 60 ? 'var(--amber)' : 'var(--accent)';

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

    document.getElementById('cmdInput').addEventListener('keydown', e => {
      if (e.key === 'Enter') sendCmd();
    });

    async function refreshLogStats() {
      try {
        const res = await fetch('/log/stats');
        const d = await res.json();

        if (d.error) {
          document.getElementById('rowCountText').textContent = 'Storage unavailable';
          document.getElementById('storageText').textContent = 'Filesystem not mounted';
          document.getElementById('logBar').style.width = '0%';
          return;
        }

        const pct = Math.min((d.rows / d.max) * 100, 100);
        const usedKB = (d.used_bytes / 1024).toFixed(1);
        const totalKB = (d.total_bytes / 1024).toFixed(0);

        document.getElementById('rowCountText').textContent = `${d.rows} / ${d.max} rows`;
        document.getElementById('storageText').textContent = `${usedKB} KB / ${totalKB} KB used`;

        const bar = document.getElementById('logBar');
        bar.style.width = pct + '%';
        bar.style.background = pct >= 90 ? 'var(--red)' : pct >= 70 ? 'var(--amber)' : 'var(--accent)';
      } catch (e) {
        document.getElementById('rowCountText').textContent = 'Storage unavailable';
      }
    }

    async function clearLog() {
      if (!confirm('Clear the entire activity log from flash? This cannot be undone.')) return;
      try {
        await fetch('/log/clear', { method:'POST' });
        logEntry('Flash log cleared', 'info');
        refreshLogStats();
      } catch (e) {
        logEntry('Failed to clear log', 'alert');
      }
    }

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
//  SECTION 10 — SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
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

  if (TEST_MODE) {
    Serial.println("[CONFIG] TEST MODE — simulated K66F data");
    xTaskCreate(testModeTask, "testMode", 4096, NULL, 1, NULL);
  } else {
    Serial.println("[CONFIG] LIVE MODE — UART from K66F");
    K66Serial.begin(UART_BAUD, SERIAL_8N1, K66_RX_PIN, K66_TX_PIN);
    Serial.printf("[UART] K66F UART ready on RX=%d TX=%d\n", K66_RX_PIN, K66_TX_PIN);
    xTaskCreate(uartReadTask, "uartRead", 6144, NULL, 1, NULL);
  }

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
//  SECTION 11 — MAIN LOOP
// ============================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  server.handleClient();
}