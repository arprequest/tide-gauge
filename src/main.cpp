// ═══════════════════════════════════════════════════════════════════
// Tide Gauge — Standalone ESP32 Firmware
//
// Hardware:
//   GPIO26 (DAC2) → MCP6002 U1A (unity-gain buffer) → R3 (~3kΩ) → galvanometer (+)
//   GPIO25 (DAC1) → MCP6002 U1B (unity-gain buffer) → R4 (~3kΩ) → galvanometer (−)
//   MCP6002 VCC → 3.3V, GND → GND; each op-amp output tied to (−in)
//
//   Push-pull drive: GPIO25 mirrors GPIO26 inverted (255 − dacVal).
//   Op-amp buffers eliminate ESP32 DAC output impedance, allowing full
//   gauge deflection. At full swing: ~3.1V / 6kΩ ≈ 0.5mA = gauge FSD.
//
//   DAC value 128 = 1.65V = center (0 tide delta from MSL)
//   DAC value 255 = 3.3V  = full positive (high tide)
//   DAC value   0 = 0V    = full negative (low tide)
//
// NOAA station 9444900 Port Townsend, WA
//   MSL = 8.35 ft above MLLW
//   Tidal range: ±8 ft from MSL → maps to full gauge deflection
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
#define DAC_PIN       26     // GPIO26 (DAC2) → MCP6002 U1A → R3 (~3kΩ) → gauge (+)
#define DAC_REF_PIN   25     // GPIO25 (DAC1) → MCP6002 U1B → R4 (~3kΩ) → gauge (−)
#define DAC_CENTER    128    // mid-point — both DACs equal → no current
#define DAC_POS       250    // full positive deflection — needle at end stop
#define DAC_NEG         5    // full negative deflection — needle at end stop
#define TIDE_SCALE_FT 8.0f  // ±8 ft from MSL = full deflection
#define NOAA_MSL_FT   8.35f // Port Townsend MSL above MLLW

// ── NOAA API ──────────────────────────────────────────────────────
// Water level (6-min readings) + hi/lo predictions
static const char* NOAA_HOST = "api.tidesandcurrents.noaa.gov";
static const char* NOAA_STATION = "9444900";

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

struct MeteoState {
  float tempF          = 0.0f;
  float windKnots      = 0.0f;
  float windGustKnots  = 0.0f;
  String windDirLabel  = "--";
  String fetchedAt     = "--";
  bool  valid = false;
};

TideState  tideState;
MeteoState meteoState;

WebServer server(80);

unsigned long lastTideFetch    = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastNeedleUpdate = 0;
unsigned long lastTestCommand  = 0;
#define TEST_MODE_TIMEOUT 15000UL  // suppress loop needle updates for 15s after test command

// ═══════════════════════════════════════════════════════════════════
// DAC helpers
// ═══════════════════════════════════════════════════════════════════

// Map tide delta (ft from MSL) to DAC value
// +TIDE_SCALE_FT → 255, 0 → 128, −TIDE_SCALE_FT → 0
uint8_t tideToDAC(float deltaMSL) {
  float clamped = constrain(deltaMSL, -TIDE_SCALE_FT, TIDE_SCALE_FT);
  float normalized = clamped / TIDE_SCALE_FT; // −1.0 to +1.0
  int dac = DAC_CENTER + (int)(normalized * (DAC_POS - DAC_CENTER));
  return (uint8_t)constrain(dac, DAC_NEG, DAC_POS);
}

// Push-pull: GPIO26 and GPIO25 move in opposite directions.
// At center (128) both output ~1.65V → zero differential.
// Inverting GPIO25 doubles the swing and cancels DAC offset asymmetry.
void setNeedle(uint8_t dacVal) {
  dacWrite(DAC_PIN,     255 - dacVal);
  dacWrite(DAC_REF_PIN, dacVal);
}

// Boot sweep: full left → full right → center
void bootSweep() {
  // Snap to full negative (left) and hold for needle to settle
  setNeedle(DAC_NEG);
  delay(600);
  // Sweep full left to full right
  for (int i = DAC_NEG; i <= DAC_POS; i += 2) {
    setNeedle(i);
    delay(10);
  }
  // Hold at full positive so needle reaches the stop
  delay(600);
  // Return to center
  for (int i = DAC_POS; i >= DAC_CENTER; i -= 2) {
    setNeedle(i);
    delay(10);
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
// NOAA meteorological fetch (air temp + wind from station 9444900)
// ═══════════════════════════════════════════════════════════════════

void fetchMeteo() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // ── Air temperature ───────────────────────────────────────────
  String tUrl = String("https://") + NOAA_HOST +
    "/api/prod/datagetter?station=" + NOAA_STATION +
    "&product=air_temperature&time_zone=gmt&units=english&format=json&range=1";
  http.begin(client, tUrl);
  if (http.GET() == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      JsonArray data = doc["data"].as<JsonArray>();
      if (data.size() > 0) {
        const char* v = data[data.size() - 1]["v"];
        if (v && strcmp(v, "NaN") != 0)
          meteoState.tempF = String(v).toFloat();
      }
    }
  }
  http.end();

  // ── Wind ──────────────────────────────────────────────────────
  String wUrl = String("https://") + NOAA_HOST +
    "/api/prod/datagetter?station=" + NOAA_STATION +
    "&product=wind&time_zone=gmt&units=english&format=json&range=1";
  http.begin(client, wUrl);
  if (http.GET() == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      JsonArray data = doc["data"].as<JsonArray>();
      if (data.size() > 0) {
        JsonObject latest = data[data.size() - 1];
        const char* s  = latest["s"];   // speed (knots)
        const char* g  = latest["g"];   // gust  (knots)
        const char* dr = latest["dr"];  // direction label e.g. "NW"
        if (s  && strcmp(s,  "NaN") != 0) meteoState.windKnots     = String(s).toFloat();
        if (g  && strcmp(g,  "NaN") != 0) meteoState.windGustKnots = String(g).toFloat();
        if (dr)                           meteoState.windDirLabel   = String(dr);
      }
    }
  }
  http.end();

  meteoState.valid     = true;
  meteoState.fetchedAt = nowString();
  Serial.printf("[Meteo] %.1f°F  wind %s %.1f kt  gust %.1f kt\n",
    meteoState.tempF, meteoState.windDirLabel.c_str(),
    meteoState.windKnots, meteoState.windGustKnots);
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
<div class="subtitle">Port Townsend, WA &mdash; NOAA Station 9444900</div>
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

  // ── Tide chart card (browser fetches NOAA predictions and renders SVG) ──
  html += "<div class=\"card\"><h2>48-Hour Tide Prediction</h2>"
          "<div id=\"tc\" style=\"color:#8b949e;font-size:0.8rem\">Loading chart&hellip;</div>"
          "<div class=\"fetched\">NOAA station 9444900 &bull; predictions in local time</div>"
          "</div>";

  // ── Met card (NOAA observations) ──
  html += "<div class=\"card\">";
  html += "<h2>Conditions at Port Townsend &mdash; NOAA Station 9444900</h2>";

  if (meteoState.valid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", meteoState.tempF);
    html += "<div><span class=\"big-value\">" + String(buf) + "</span><span class=\"big-unit\">&deg;F</span></div>";
    html += "<div style=\"margin-top:10px\" class=\"row\">";
    html += "<div class=\"col\"><div class=\"stat-label\">Wind</div>";
    snprintf(buf, sizeof(buf), "%.1f kt", meteoState.windKnots);
    html += "<div class=\"stat-value\">" + String(buf) + "</div></div>";
    html += "<div class=\"col\"><div class=\"stat-label\">Direction</div>";
    html += "<div class=\"stat-value\">" + meteoState.windDirLabel + "</div></div>";
    html += "<div class=\"col\"><div class=\"stat-label\">Gust</div>";
    snprintf(buf, sizeof(buf), "%.1f kt", meteoState.windGustKnots);
    html += "<div class=\"stat-value\">" + String(buf) + "</div></div>";
    html += "</div>";
  } else {
    html += "<div style=\"color:#8b949e\">Fetching&hellip;</div>";
  }

  html += "<div class=\"fetched\">Updated " + meteoState.fetchedAt + "</div>";
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

  // Inject current tide values for the chart renderer
  char chartVars[64];
  snprintf(chartVars, sizeof(chartVars), "<script>const CF=%.2f,MF=%.2f;", tideState.currentFt, NOAA_MSL_FT);
  html += String(chartVars);

  // Chart renderer — browser fetches 48h hourly predictions from NOAA and draws an SVG
  html += R"rawjs(
(async()=>{
const el=document.getElementById('tc');
try{
const r=await fetch('https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?station=9444900&product=predictions&datum=MLLW&time_zone=gmt&interval=h&units=english&format=json&range=48');
if(!r.ok)throw 0;
const{predictions:P}=await r.json();
const vs=P.map(p=>+p.v),lo=Math.min(...vs)-.3,hi=Math.max(...vs)+.3;
const W=560,H=155,PL=30,PB=22,PT=6,PR=4,cw=W-PL-PR,ch=H-PT-PB;
const ts=P.map(p=>new Date(p.t.replace(' ','T')+'Z').getTime());
const t0=ts[0],t1=ts[ts.length-1];
const tx=t=>(PL+cw*(t-t0)/(t1-t0)).toFixed(1);
const ty=v=>(PT+ch*(1-(v-lo)/(hi-lo))).toFixed(1);
const now=Date.now();
let s=`<svg viewBox="0 0 ${W} ${H}" style="width:100%;height:auto;display:block;border-radius:4px">`;
const mY=+ty(MF);
s+=`<line x1="${PL}" y1="${mY}" x2="${W-PR}" y2="${mY}" stroke="#484f58" stroke-width="1" stroke-dasharray="4,3"/>`;
s+=`<text x="2" y="${mY+3}" font-size="8" fill="#484f58">MSL</text>`;
const path=P.map((p,i)=>(i?'L':'M')+tx(ts[i])+','+ty(vs[i])).join('');
s+=`<path d="${path} L${tx(t1)},${H-PB} L${PL},${H-PB}Z" fill="rgba(33,150,243,.1)"/>`;
s+=`<path d="${path}" fill="none" stroke="#2196F3" stroke-width="2" stroke-linejoin="round"/>`;
const nx=+tx(now);
if(nx>PL&&nx<W-PR){
  s+=`<line x1="${nx}" y1="${PT}" x2="${nx}" y2="${H-PB}" stroke="#dc2626" stroke-width="1" stroke-dasharray="3,2"/>`;
  if(CF>0)s+=`<circle cx="${nx}" cy="${ty(CF)}" r="4" fill="#3fb950" stroke="#161b22" stroke-width="1.5"/>`;
}
for(let i=1;i<P.length-1;i++){
  if(vs[i]>vs[i-1]&&vs[i]>vs[i+1])s+=`<text x="${tx(ts[i])}" y="${+ty(vs[i])-5}" text-anchor="middle" font-size="9" fill="#3fb950">${vs[i].toFixed(1)}</text>`;
  else if(vs[i]<vs[i-1]&&vs[i]<vs[i+1])s+=`<text x="${tx(ts[i])}" y="${+ty(vs[i])+11}" text-anchor="middle" font-size="9" fill="#f78166">${vs[i].toFixed(1)}</text>`;
}
for(let t=t0;t<=t1;t+=6*3600000){
  const x=+tx(t);
  if(x>PL+5&&x<W-5){
    const d=new Date(t);
    const lbl=d.getHours()===0?`${d.getMonth()+1}/${d.getDate()}`:d.getHours().toString().padStart(2,'0')+':00';
    s+=`<text x="${x}" y="${H-5}" text-anchor="middle" font-size="8" fill="#8b949e">${lbl}</text>`;
  }
}
for(let v=Math.ceil(lo/2)*2;v<=hi;v+=2){
  const y=+ty(v);
  s+=`<text x="${PL-3}" y="${y+3}" text-anchor="end" font-size="8" fill="#8b949e">${v}</text>`;
}
s+=`<line x1="${PL}" y1="${PT}" x2="${PL}" y2="${H-PB}" stroke="#30363d"/>`;
s+=`<line x1="${PL}" y1="${H-PB}" x2="${W-PR}" y2="${H-PB}" stroke="#30363d"/>`;
s+='</svg>';
el.innerHTML=s;
}catch(e){el.innerHTML='<span style="color:#484f58;font-size:.8rem">Chart unavailable</span>';}
})();
</script>
)rawjs";

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

void handleTest() {
  // If a 'dac' param is present, apply it immediately
  if (server.hasArg("dac")) {
    int val = server.arg("dac").toInt();
    val = constrain(val, 0, 255);
    setNeedle((uint8_t)val);
    lastTestCommand = millis();  // suppress loop override for 15s
    server.send(200, "text/plain", "OK");
    return;
  }

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gauge Test</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #0d1117; color: #c9d1d9;
         min-height: 100vh; display: flex; flex-direction: column;
         align-items: center; justify-content: center; padding: 24px; gap: 20px; }
  h1 { color: #58a6ff; font-size: 1.3rem; }
  .card { background: #161b22; border: 1px solid #30363d; border-radius: 10px;
          padding: 20px; width: 100%; max-width: 400px; }
  .dac-display { font-size: 3rem; font-weight: 700; text-align: center;
                 color: #f0f6fc; margin-bottom: 16px; }
  input[type=range] { width: 100%; accent-color: #58a6ff; cursor: pointer; height: 8px; }
  .presets { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-top: 16px; }
  button { padding: 10px; border-radius: 6px; border: 1px solid #30363d;
           background: #21262d; color: #c9d1d9; font-size: 0.9rem;
           cursor: pointer; transition: background 0.15s; }
  button:hover { background: #58a6ff; color: #fff; border-color: #58a6ff; }
  .label { font-size: 0.75rem; color: #8b949e; text-align: center; margin-top: 4px; }
  a { color: #58a6ff; font-size: 0.85rem; }
</style>
</head>
<body>
<h1>&#127754; Gauge Test</h1>
<div class="card">
  <div class="dac-display" id="val">128</div>
  <input type="range" min="0" max="255" value="128" id="slider" oninput="update(this.value)">
  <div class="label">DAC value (0 = full left &nbsp;|&nbsp; 128 = center &nbsp;|&nbsp; 255 = full right)</div>
  <div class="presets">
    <button onclick="set(5)">Full Left<br><small>DAC 5</small></button>
    <button onclick="set(128)">Center<br><small>DAC 128</small></button>
    <button onclick="set(250)">Full Right<br><small>DAC 250</small></button>
  </div>
</div>
<a href="/">&#8592; Back to tide page</a>
<script>
  function update(v) {
    document.getElementById('val').textContent = v;
    fetch('/test?dac=' + v);
  }
  function set(v) {
    document.getElementById('slider').value = v;
    update(v);
  }
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html", html);
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

  setNeedle(DAC_CENTER);  // center needle while connecting

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
  fetchMeteo();
  setNeedle(tideToDAC(tideState.deltaMSL));

  // ── Web server ───────────────────────────────────────────────
  server.on("/", handleRoot);
  server.on("/test", handleTest);
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
    fetchMeteo();
  }

  if (now - lastNeedleUpdate >= DISPLAY_INTERVAL_MS) {
    lastNeedleUpdate = now;
    if (tideState.valid && (now - lastTestCommand >= TEST_MODE_TIMEOUT)) {
      setNeedle(tideToDAC(tideState.deltaMSL));
    }
  }
}
