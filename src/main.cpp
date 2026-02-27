// ═══════════════════════════════════════════════════════════════════
// Tide Gauge — Standalone ESP32 Firmware
//
// Hardware:
//   GPIO26 (DAC2) → ~3kΩ → galvanometer (+)
//   Voltage divider (3.3V → two 10kΩ → GND, mid = 1.65V) → ~3kΩ → galvanometer (−)
//
//   DAC value 128 = 1.65V = center (0 tide delta from MSL)
//   DAC value 255 = 3.3V  = full positive (high tide)
//   DAC value   0 = 0V    = full negative (low tide)
//
// NOAA station 9444900 Port Townsend, WA
//   MSL = 8.35 ft above MLLW
//   Tidal range: ±8 ft from MSL → maps to ±127 DAC counts
//
// Web page: http://<device-ip>/
//   Shows current tide, next high/low, weather, WiFi info, reset button
// ═══════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>

// ── Pin / hardware constants ──────────────────────────────────────
#define DAC_PIN       26
#define DAC_CENTER    128    // 1.65V mid-point
#define TIDE_SCALE_FT 8.0f  // ±8 ft from MSL = full deflection
#define NOAA_MSL_FT   8.35f // Port Townsend MSL above MLLW

// ── NOAA API ──────────────────────────────────────────────────────
// Water level (6-min readings) + hi/lo predictions
static const char* NOAA_HOST = "api.tidesandcurrents.noaa.gov";
static const char* NOAA_STATION = "9444900";

// ── Open-Meteo API ────────────────────────────────────────────────
static const float LAT = 48.115f;
static const float LON = -122.760f;

// ── Poll intervals ────────────────────────────────────────────────
#define TIDE_INTERVAL_MS    360000UL  //  6 minutes
#define WEATHER_INTERVAL_MS 900000UL  // 15 minutes
#define DISPLAY_INTERVAL_MS   5000UL  //  5 seconds (needle update)

// ── Global state ─────────────────────────────────────────────────
struct TideState {
  float currentFt   = 0.0f;     // current water level above MLLW
  float deltaMSL    = 0.0f;     // current - MSL (positive = above MSL)
  String nextEventType = "--";  // "High" or "Low"
  float nextEventFt = 0.0f;
  String nextEventTime = "--";
  String fetchedAt = "--";
  bool  valid = false;
};

struct WeatherState {
  float tempF       = 0.0f;
  float windMph     = 0.0f;
  float windDirDeg  = 0.0f;
  String condition  = "--";
  String fetchedAt  = "--";
  bool  valid = false;
};

TideState   tideState;
WeatherState weatherState;

WebServer server(80);

unsigned long lastTideFetch    = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastNeedleUpdate = 0;

// ═══════════════════════════════════════════════════════════════════
// DAC helpers
// ═══════════════════════════════════════════════════════════════════

// Map tide delta (ft from MSL) to DAC value
// +TIDE_SCALE_FT → 255, 0 → 128, −TIDE_SCALE_FT → 0
uint8_t tideToDAC(float deltaMSL) {
  float clamped = constrain(deltaMSL, -TIDE_SCALE_FT, TIDE_SCALE_FT);
  float normalized = clamped / TIDE_SCALE_FT; // −1.0 to +1.0
  int dac = DAC_CENTER + (int)(normalized * 127.0f);
  return (uint8_t)constrain(dac, 0, 255);
}

void setNeedle(uint8_t dacVal) {
  dacWrite(DAC_PIN, dacVal);
}

// Boot sweep: full left → full right → center
void bootSweep() {
  // Snap to full negative (left)
  setNeedle(0);
  delay(200);
  // Sweep full left to full right
  for (int i = 0; i <= 255; i += 3) {
    setNeedle(i);
    delay(12);
  }
  delay(150);
  // Return to center
  for (int i = 255; i >= DAC_CENTER; i -= 3) {
    setNeedle(i);
    delay(12);
  }
  setNeedle(DAC_CENTER);
}

// ═══════════════════════════════════════════════════════════════════
// Time helpers
// ═══════════════════════════════════════════════════════════════════

// Returns current UTC time as "YYYYMMDD HH:MM" for NOAA API
String noaaDateParam(int offsetDays = 0) {
  time_t now = time(nullptr) + offsetDays * 86400;
  struct tm* t = gmtime(&now);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d%02d%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}

// Human-readable local time string (Pacific, no DST handling — display only)
String nowString() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

// ═══════════════════════════════════════════════════════════════════
// NOAA fetch
// ═══════════════════════════════════════════════════════════════════

void fetchTide() {
  WiFiClientSecure client;
  client.setInsecure();

  // ── Current water level ──────────────────────────────────────
  // Get latest 6-minute observation
  String url = String("https://") + NOAA_HOST +
    "/api/prod/datagetter?station=" + NOAA_STATION +
    "&product=water_level&datum=MLLW&time_zone=gmt&units=english"
    "&format=json&range=1";

  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      // Latest reading is last element in data array
      JsonArray data = doc["data"].as<JsonArray>();
      if (data.size() > 0) {
        JsonObject latest = data[data.size() - 1];
        float v = String(latest["v"].as<const char*>()).toFloat();
        tideState.currentFt = v;
        tideState.deltaMSL  = v - NOAA_MSL_FT;
        tideState.valid     = true;
      }
    }
  }
  http.end();

  // ── Next hi/lo prediction ────────────────────────────────────
  String begin_date = noaaDateParam(0);
  String end_date   = noaaDateParam(2);

  String url2 = String("https://") + NOAA_HOST +
    "/api/prod/datagetter?station=" + NOAA_STATION +
    "&product=predictions&datum=MLLW&time_zone=gmt&units=english"
    "&format=json&interval=hilo"
    "&begin_date=" + begin_date + "&end_date=" + end_date;

  http.begin(client, url2);
  code = http.GET();

  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(
      JsonDocument()  // accept all
    ));
    // Re-parse since stream may be consumed; use String approach instead
    http.end();

    // Re-fetch as string for reliable parsing
    http.begin(client, url2);
    code = http.GET();
    if (code == 200) {
      String body = http.getString();
      JsonDocument doc2;
      deserializeJson(doc2, body);

      time_t now = time(nullptr);
      JsonArray predictions = doc2["predictions"].as<JsonArray>();

      for (JsonObject p : predictions) {
        // Parse "YYYY-MM-DD HH:MM" in UTC
        String t_str = p["t"].as<String>();
        struct tm ptm = {};
        sscanf(t_str.c_str(), "%4d-%2d-%2d %2d:%2d",
               &ptm.tm_year, &ptm.tm_mon, &ptm.tm_mday,
               &ptm.tm_hour, &ptm.tm_min);
        ptm.tm_year -= 1900;
        ptm.tm_mon  -= 1;
        time_t pt = mktime(&ptm); // mktime interprets as local, adjust
        // mktime uses local TZ; add UTC offset. For display it's fine.

        if (pt > now) {
          tideState.nextEventType = p["type"].as<String>() == "H" ? "High" : "Low";
          tideState.nextEventFt   = String(p["v"].as<const char*>()).toFloat();
          // Format time nicely
          char buf[10];
          snprintf(buf, sizeof(buf), "%02d:%02d UTC", ptm.tm_hour, ptm.tm_min);
          tideState.nextEventTime = String(buf);
          break;
        }
      }
    }
  }
  http.end();

  tideState.fetchedAt = nowString();
  Serial.printf("[Tide] %.2f ft (delta MSL: %+.2f ft), next: %s %.2f ft @ %s\n",
    tideState.currentFt, tideState.deltaMSL,
    tideState.nextEventType.c_str(), tideState.nextEventFt,
    tideState.nextEventTime.c_str());
}

// ═══════════════════════════════════════════════════════════════════
// Open-Meteo weather fetch
// ═══════════════════════════════════════════════════════════════════

static const char* WMO_CODE_MAP[] = {
  // index = WMO code, but sparse — use a switch below
};

String wmoDescription(int code) {
  switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 61: case 63: case 65: return "Rain";
    case 71: case 73: case 75: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 95: return "Thunderstorm";
    default: return "Unknown (" + String(code) + ")";
  }
}

String windDirection(float deg) {
  const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
  int idx = (int)((deg + 22.5f) / 45.0f) % 8;
  return String(dirs[idx]);
}

void fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();

  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.3f&longitude=%.3f"
    "&current=temperature_2m,weathercode,windspeed_10m,winddirection_10m"
    "&temperature_unit=fahrenheit&windspeed_unit=mph&timezone=America%%2FLos_Angeles",
    LAT, LON);

  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      JsonObject cur = doc["current"];
      weatherState.tempF      = cur["temperature_2m"].as<float>();
      weatherState.windMph    = cur["windspeed_10m"].as<float>();
      weatherState.windDirDeg = cur["winddirection_10m"].as<float>();
      weatherState.condition  = wmoDescription(cur["weathercode"].as<int>());
      weatherState.valid      = true;
    }
  }
  http.end();

  weatherState.fetchedAt = nowString();
  Serial.printf("[Weather] %.1f°F, %s %.1f mph, %s\n",
    weatherState.tempF,
    windDirection(weatherState.windDirDeg).c_str(),
    weatherState.windMph,
    weatherState.condition.c_str());
}

// ═══════════════════════════════════════════════════════════════════
// Web server
// ═══════════════════════════════════════════════════════════════════

// Tide bar: maps deltaMSL to 0–100% (center = 50%)
int tideBarPercent() {
  float pct = 50.0f + (tideState.deltaMSL / TIDE_SCALE_FT) * 50.0f;
  return (int)constrain(pct, 0.0f, 100.0f);
}

void handleRoot() {
  String ip = WiFi.localIP().toString();
  String ssid = WiFi.SSID();
  int bar = tideBarPercent();
  bool aboveMSL = tideState.deltaMSL >= 0;
  String barColor = aboveMSL ? "#2196F3" : "#78909C";

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>Tide Gauge</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
         background: #0d1117; color: #c9d1d9; min-height: 100vh; padding: 20px; }
  h1 { color: #58a6ff; font-size: 1.4rem; margin-bottom: 4px; }
  .subtitle { color: #8b949e; font-size: 0.85rem; margin-bottom: 20px; }
  .card { background: #161b22; border: 1px solid #30363d; border-radius: 10px;
          padding: 16px; margin-bottom: 14px; }
  .card h2 { font-size: 0.8rem; text-transform: uppercase; letter-spacing: 0.08em;
             color: #8b949e; margin-bottom: 12px; }
  .big-value { font-size: 2.5rem; font-weight: 700; color: #f0f6fc; line-height: 1; }
  .big-unit  { font-size: 1rem; color: #8b949e; margin-left: 4px; }
  .delta     { font-size: 1rem; margin-top: 4px; }
  .pos { color: #3fb950; }
  .neg { color: #f78166; }
  .bar-wrap { background: #21262d; border-radius: 4px; height: 18px;
              margin: 12px 0; position: relative; overflow: hidden; }
  .bar-fill { height: 100%; border-radius: 4px; transition: width 0.5s; }
  .bar-mid  { position: absolute; left: 50%; top: 0; bottom: 0;
              width: 2px; background: #484f58; }
  .bar-label { font-size: 0.75rem; color: #8b949e; display: flex;
               justify-content: space-between; }
  .row { display: flex; gap: 12px; }
  .row .col { flex: 1; }
  .stat-label { font-size: 0.75rem; color: #8b949e; margin-bottom: 2px; }
  .stat-value { font-size: 1.05rem; font-weight: 600; color: #e6edf3; }
  .wifi-row { display: flex; justify-content: space-between; align-items: center;
              font-size: 0.9rem; padding: 4px 0; border-bottom: 1px solid #21262d; }
  .wifi-row:last-child { border-bottom: none; }
  .wifi-key { color: #8b949e; }
  .wifi-val { color: #e6edf3; font-weight: 500; }
  .btn { display: inline-block; margin-top: 12px; padding: 8px 18px;
         background: #21262d; color: #f85149; border: 1px solid #f85149;
         border-radius: 6px; text-decoration: none; font-size: 0.85rem;
         cursor: pointer; }
  .btn:hover { background: #f85149; color: #fff; }
  .fetched { font-size: 0.72rem; color: #484f58; margin-top: 8px; text-align: right; }
  .gauge-vis { display: flex; align-items: center; justify-content: center;
               gap: 8px; margin: 8px 0; }
  .gauge-tick { width: 3px; background: #30363d; border-radius: 2px; }
  .needle-label { font-size: 0.7rem; color: #484f58; }
</style>
</head>
<body>
<h1>&#127754; Tide Gauge</h1>
<div class="subtitle">Port Townsend, WA &mdash; Station 9444900 &mdash; Freeland WA reference</div>
)rawhtml";

  // ── Tide card ──
  html += "<div class=\"card\">";
  html += "<h2>Current Tide</h2>";

  if (tideState.valid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", tideState.currentFt);
    html += "<div><span class=\"big-value\">" + String(buf) + "</span><span class=\"big-unit\">ft above MLLW</span></div>";

    float d = tideState.deltaMSL;
    String dClass = (d >= 0) ? "pos" : "neg";
    snprintf(buf, sizeof(buf), "%+.2f", d);
    html += "<div class=\"delta " + dClass + "\">MSL delta: " + String(buf) + " ft</div>";

    // Tide bar
    html += "<div class=\"bar-wrap\"><div class=\"bar-fill\" style=\"width:" +
            String(bar) + "%;background:" + barColor + "\"></div><div class=\"bar-mid\"></div></div>";
    html += "<div class=\"bar-label\"><span>Low (&minus;8 ft)</span><span>MSL</span><span>High (+8 ft)</span></div>";

    // Next event
    html += "<div style=\"margin-top:12px\" class=\"row\">";
    html += "<div class=\"col\"><div class=\"stat-label\">Next " + tideState.nextEventType + "</div>";
    snprintf(buf, sizeof(buf), "%.2f ft", tideState.nextEventFt);
    html += "<div class=\"stat-value\">" + String(buf) + "</div></div>";
    html += "<div class=\"col\"><div class=\"stat-label\">At</div>";
    html += "<div class=\"stat-value\">" + tideState.nextEventTime + "</div></div>";
    html += "</div>";
  } else {
    html += "<div style=\"color:#8b949e\">Fetching&hellip;</div>";
  }

  html += "<div class=\"fetched\">Updated " + tideState.fetchedAt + "</div>";
  html += "</div>";

  // ── Weather card ──
  html += "<div class=\"card\">";
  html += "<h2>Current Weather &mdash; Freeland WA</h2>";

  if (weatherState.valid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", weatherState.tempF);
    html += "<div><span class=\"big-value\">" + String(buf) + "</span><span class=\"big-unit\">&deg;F</span></div>";
    html += "<div style=\"margin-top:6px;color:#8b949e\">" + weatherState.condition + "</div>";
    html += "<div style=\"margin-top:10px\" class=\"row\">";
    html += "<div class=\"col\"><div class=\"stat-label\">Wind</div>";
    snprintf(buf, sizeof(buf), "%.1f mph", weatherState.windMph);
    html += "<div class=\"stat-value\">" + String(buf) + "</div></div>";
    html += "<div class=\"col\"><div class=\"stat-label\">Direction</div>";
    html += "<div class=\"stat-value\">" + windDirection(weatherState.windDirDeg) +
            " (" + String((int)weatherState.windDirDeg) + "&deg;)</div></div>";
    html += "</div>";
  } else {
    html += "<div style=\"color:#8b949e\">Fetching&hellip;</div>";
  }

  html += "<div class=\"fetched\">Updated " + weatherState.fetchedAt + "</div>";
  html += "</div>";

  // ── WiFi card ──
  html += "<div class=\"card\">";
  html += "<h2>WiFi &amp; Device</h2>";
  html += "<div class=\"wifi-row\"><span class=\"wifi-key\">SSID</span><span class=\"wifi-val\">" + ssid + "</span></div>";
  html += "<div class=\"wifi-row\"><span class=\"wifi-key\">IP Address</span><span class=\"wifi-val\">" + ip + "</span></div>";
  html += "<div class=\"wifi-row\"><span class=\"wifi-key\">RSSI</span><span class=\"wifi-val\">" + String(WiFi.RSSI()) + " dBm</span></div>";
  char dacBuf[8]; snprintf(dacBuf, sizeof(dacBuf), "%d", tideToDAC(tideState.deltaMSL));
  html += "<div class=\"wifi-row\"><span class=\"wifi-key\">DAC output</span><span class=\"wifi-val\">" + String(dacBuf) + " / 255</span></div>";
  html += "<a class=\"btn\" href=\"/reset\">&#x21BA; Reset WiFi</a>";
  html += "</div>";

  html += "<div style=\"font-size:0.7rem;color:#484f58;text-align:center\">Page auto-refreshes every 30 s</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleReset() {
  server.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:40px'>"
    "<h2>WiFi credentials cleared.</h2>"
    "<p>Device will restart into configuration mode.<br>"
    "Connect to <strong>TideGauge</strong> AP to reconfigure.</p>"
    "</body></html>");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void handle404() {
  server.send(404, "text/plain", "Not found");
}

// ═══════════════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n[TideGauge] Booting...");

  dacWrite(DAC_PIN, DAC_CENTER);  // center needle while connecting

  // ── WiFiManager ──────────────────────────────────────────────
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);  // 3-minute portal timeout
  wm.setConnectTimeout(20);

  // Auto-connect; if it fails, open the config AP
  bool connected = wm.autoConnect("TideGauge");
  if (!connected) {
    Serial.println("[WiFi] Config portal timed out, restarting...");
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected: %s  IP: %s\n",
    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  // ── NTP ──────────────────────────────────────────────────────
  configTime(-8 * 3600, 3600, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing");
  time_t now = 0;
  int attempts = 0;
  while (now < 1000000000L && attempts < 20) {
    delay(500);
    Serial.print(".");
    time(&now);
    attempts++;
  }
  Serial.println(now > 1000000000L ? " OK" : " timeout (continuing)");

  // ── Boot sweep ───────────────────────────────────────────────
  bootSweep();

  // ── Initial data fetch ───────────────────────────────────────
  fetchTide();
  fetchWeather();
  setNeedle(tideToDAC(tideState.deltaMSL));

  // ── Web server ───────────────────────────────────────────────
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.onNotFound(handle404);
  server.begin();
  Serial.println("[HTTP] Server started");
}

// ═══════════════════════════════════════════════════════════════════
// Loop
// ═══════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (now - lastTideFetch >= TIDE_INTERVAL_MS) {
    lastTideFetch = now;
    fetchTide();
  }

  if (now - lastWeatherFetch >= WEATHER_INTERVAL_MS) {
    lastWeatherFetch = now;
    fetchWeather();
  }

  if (now - lastNeedleUpdate >= DISPLAY_INTERVAL_MS) {
    lastNeedleUpdate = now;
    if (tideState.valid) {
      setNeedle(tideToDAC(tideState.deltaMSL));
    }
  }
}
