# Web Interface Redesign Plan

## Target Hardware

| Component | Part | Cost |
|-----------|------|------|
| Display | Waveshare 7inch LCD for Pi (1024×600 IPS, 60Hz, no touch) | $30 |
| Computer | Raspberry Pi Zero 2W | $15 |
| Storage | MicroSD 16GB+ | $8 |
| **Total** | | **~$55** |

The Pi Zero 2W mounts behind the display, runs Raspbian, and opens Chromium in
kiosk mode pointed at `http://tidegauge.local/`. The ESP32 is unchanged — it
continues fetching tide data, driving the gauge, and serving the web page.

---

## Physical Enclosure

- Galvanometer (~4.5" across) and 7" display **side by side** in a single box
- Wall-mounted or table-top
- Display is purely informational — no touch interaction needed
- Viewed from approximately 2–4 feet

---

## Web Page Design Constraints

| Property | Value |
|----------|-------|
| Resolution | 1024 × 600 px fixed |
| Orientation | Landscape |
| Scrolling | None — everything visible at once |
| Interaction | None (display only) |
| Text size | Larger than typical web — readable at ~3 ft |
| Refresh | Auto-refresh every 30 s (existing behavior) |

---

## Layout

```
┌─────────────────────────────────────────────────────────────────┐  1024px
│  [Logo]  SPYGLASS BEACH HOUSE         Port Townsend, WA  ~70px  │
├───────────────────┬─────────────────────────┬───────────────────┤
│                   │                         │                   │
│  CURRENT TIDE     │   48-HR TIDE CHART      │   CONDITIONS      │
│                   │   (dominant element,    │                   │
│  big number       │    ~480px wide)         │   temp °F         │
│  delta from MSL   │                         │   wind / gust     │
│  tide bar         │                         │   direction       │
│                   │                         │                   │
│  NEXT HI/LO       │                         │                   │
│  time + height    │                         │                   │
│                   │                         │                   │
└───────────────────┴─────────────────────────┴───────────────────┘
       ~220px                ~500px                  ~280px         530px
```

---

## Visual Design

**Color palette** — pulled directly from the Spyglass Beach House logo:

| Role | Color |
|------|-------|
| Primary / navy | `#1a4f6e` |
| Accent / teal | `#2e8fa3` |
| Background | White or warm cream `#fafaf7` |
| Text | `#1a2e38` (near-black) |
| Positive tide | `#2e8fa3` (teal) |
| Negative tide | `#78909c` (slate) |

**Typography:**
- Station name / header: serif or slab-serif (nautical feel)
- Data values: clean sans-serif, bold, large
- Labels: small caps or spaced uppercase

**Motifs:**
- Subtle wave SVG divider between header and content
- Logo top-left of header
- Thin teal top-border on data cards (matches current wiring doc style)
- Tide chart fill uses teal with low opacity below the curve

**Chart:**
- 48-hour prediction curve (browser fetches NOAA directly — existing behavior)
- High/low labels in green/red
- Current time as vertical dashed line
- Current level as green dot
- MSL as dashed horizontal

---

## Implementation Notes

- Logo PNG embedded as base64 in the HTML (no separate file to serve)
- Fixed viewport `<meta name="viewport" content="width=1024">` so layout
  doesn't reflow on the Pi's browser
- No external font CDN — embed or use system fonts for offline reliability
- Pi kiosk setup: `chromium-browser --kiosk --noerrdialogs
  --disable-infobars http://tidegauge.local/`
- Consider `--app=http://tidegauge.local/` flag to hide all browser chrome

---

## Open Questions

- [ ] Enclosure material — wood, painted MDF, or metal?
- [ ] Landscape or portrait orientation for the enclosure?
- [ ] Should the Spyglass name appear on the display, or just the logo mark?
- [ ] Any other data to show — barometric pressure, moon phase, tide station map?
