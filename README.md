# Tide Gauge

A standalone tide gauge built from a repurposed center-zero galvanometer and an ESP32. The needle shows the current tide level relative to Mean Sea Level — right of center means above MSL, left means below.

![Weston Model 643 galvanometer repurposed as a tide gauge]

## How It Works

The ESP32 polls the NOAA tide API every 6 minutes and maps the water level to a DAC value. Two DAC outputs drive the galvanometer in push-pull through MCP6002 op-amp unity-gain buffers, which eliminate the ESP32's high DAC output impedance and allow full-scale deflection.

```
GPIO26 → MCP6002 U1A (buffer) → R3 (~3kΩ) → gauge (+)
GPIO25 → MCP6002 U1B (buffer) → R4 (~3kΩ) → gauge (−)
```

GPIO25 always mirrors GPIO26 inverted (`255 − dacVal`), doubling the voltage swing across the coil. At full swing: ~3.1 V ÷ 6 kΩ ≈ 0.5 mA = gauge FSD.

## Hardware

| Part | Notes |
|------|-------|
| ESP32-WROOM-32 dev board | Any 38-pin ESP32 devkit |
| Weston Model 643 galvanometer | "Yards Per Second" scale, ±0.5 mA end scale, 49 Ω coil |
| MCP6002 dual op-amp | DIP-8 or SOT-23-8, rail-to-rail, unity-gain buffers |
| 2× 10 kΩ trimpot (marked 103) | R3 and R4, set to ~3 kΩ each |
| 100 nF ceramic capacitor | Bypass cap on MCP6002 VCC pin |

## Firmware

Built with [PlatformIO](https://platformio.org/).

```ini
platform  = espressif32
board     = esp32dev
framework = arduino

lib_deps =
  tzapu/WiFiManager @ ^2.0.17
  bblanchon/ArduinoJson @ ^7.0.0
```

**Flash:**
```
~/.platformio/penv/bin/pio run --target upload
```

**First boot:** The ESP32 opens a WiFi AP named `TideGauge`. Connect to it and enter your WiFi credentials in the captive portal.

## Web Interface

Once connected, the device serves a status page at its IP address showing the current tide, next high/low prediction, local weather, and a gauge test tool.

| URL | Description |
|-----|-------------|
| `http://<ip>/` | Main tide and weather dashboard |
| `http://<ip>/test` | Manual DAC control for calibration |
| `http://<ip>/reset` | Clear saved WiFi credentials |

## NOAA Station

**9444900 — Port Townsend, WA**
MSL = 8.35 ft above MLLW. The gauge maps ±8 ft from MSL to full deflection. Adjust `TIDE_SCALE_FT` in `src/main.cpp` for a different tidal range.

## Wiring Diagram

See [`docs/wiring.html`](docs/wiring.html) for the full interactive schematic, bill of materials, build steps, and firmware reference. Open it locally in a browser — it does not require a server.

## Calibration

1. Open `http://<ip>/test`
2. Set slider to 250 — needle should reach full-scale right
3. Set slider to 5 — needle should reach full-scale left
4. Adjust trimpots R3/R4 if not reaching the end stops
5. Set slider to 128 — needle should center; adjust `DAC_CENTER` in firmware if off
