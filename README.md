# miniDash

ESP8266 (Wemos D1 mini) + 1.8" ST7735 TFT dashboard firmware.

## Screens
Button cycles through the enabled screens. Each can be individually disabled from
the config page; disabled screens are skipped.

| # | Screen | Shows |
|---|--------|-------|
| 1 | **Clock** | Large time + date, weather summary, WiFi bars, sync dot, and the closest flight on the bottom line |
| 2 | **ESPHome** | Live values from the configured ESPHome sensors |
| 3 | **Forecast** | 3-day weather forecast labeled by weekday |
| 4 | **Weather Detail** | Extended current-conditions readout |
| 5 | **Monitors** | HTTP reachability status for the configured hosts |
| 6 | **Flight Radar** | Nearby aircraft on a range-ring radar (see below) |
| 7 | **System** | Heap / fragmentation, WiFi RSSI, SSID, IP, MAC, CPU, flash, uptime |

Every screen shows a top bar with WiFi signal bars, a time-sync dot (green =
synced, red = not), and a right-aligned screen counter (`idx/total`).

- **Short press** cycles to the next enabled screen.
- **Long press (≥600ms)** toggles the whole display off/on (screen contents +
  backlight). The backlight is only physically switched if the optional
  transistor is fitted (see below), but the screen is always blanked.

The display turns off automatically during configurable night hours (default
23:00–07:00).

### Flight Radar
Nearby aircraft are fetched from the [adsb.fi](https://adsb.fi) open data API
(TLS) every ~15s and drawn on a polar radar centered on your location: concentric
range rings (outer = full range, inner = half), a North marker, and one dot per
aircraft placed by bearing and distance, with a short line indicating heading.
The bottom-right shows a countdown to the next refresh. The closest aircraft is
highlighted in **yellow** and detailed in the footer (class tag, callsign,
distance, altitude, and shown/total count).

Each aircraft is classified from its ADS-B emitter category (plus the military
flag) into a short tag, which is also color-coded on the radar:

| Tag | Meaning | Source (ADS-B category) | Color |
|-----|---------|-------------------------|-------|
| `MIL` | Military | `dbFlags` military bit | red |
| `COM` | Commercial (large/heavy) | A3 / A4 / A5 | cyan |
| `HEL` | Helicopter / rotorcraft | A7 | magenta |
| `LGT` | Light general-aviation | A1 / A2 / A6 | green |
| `GLI` | Glider / sailplane | B1 | teal |
| `BAL` | Balloon / airship | B2 | orange |
| `ULT` | Ultralight / paraglider | B4 | lime |
| `UAV` | Drone / UAV | B6 | pink |
| `CIV` | Civilian / unknown | anything else | white |

Set the radar range (nm) on the config page; a range of `0` disables the screen.
Classification depends on the API providing `category` / `dbFlags` — aircraft
without those fields fall back to `CIV`.

## Wiring
| Signal | Pin |
|--------|-----|
| CS     | D1 (GPIO5) |
| DC     | D2 (GPIO4) |
| RST    | D4 (GPIO2) |
| SCK    | D5 (GPIO14) |
| MOSI   | D7 (GPIO13) |
| Button | D3 (GPIO0), pull-up; short = cycle screen, long = display on/off |
| Backlight ctrl | D6 (GPIO12), optional transistor gate/base |

### Backlight control (optional)
By default the ST7735 backlight is hardwired to 3.3V and always on. To switch it
in software, add a transistor driven by **D6 (GPIO12)** and cut the module's
backlight trace.

**This build uses a BC547 (NPN) as a low-side switch on the GND line** — the
simplest option and the one wired here:

- Cut the trace between the backlight LED cathode (LED−) and GND.
- **Collector** → backlight LED− (the GND side you just cut).
- **Emitter** → GND.
- **Base** → **D6 (GPIO12)** through a ~1kΩ resistor.
- This is **active-high**: GPIO HIGH turns the backlight ON.

There are plenty of other ways to do this depending on the parts you have and
whether you want a high-side (3.3V line) or low-side (GND line) switch:

| Type | Wiring | Cut | Active level |
|------|--------|-----|--------------|
| **NPN (BC547, 2N2222)** — used here | Collector→LED−, Emitter→GND, Base→D6 via 1kΩ | LED cathode→GND | HIGH |
| N-MOSFET (2N7000, AO3400) | Drain→LED−, Source→GND, Gate→D6 (100Ω series, 100k gate→GND) | LED cathode→GND | HIGH |
| PNP (2N2907, BC557) | Emitter→3.3V, Collector→LED+, Base via 10k pull-up + NPN driver | LED anode→3.3V | depends on driver |
| P-MOSFET + NPN | P-FET in 3.3V line, gate driven by small NPN from D6 | LED anode→3.3V | depends on driver |

Enable it on the config page ("Backlight → Enable control") and leave
**Active-high** checked (correct for the BC547 low-side wiring above). Set it to
match your wiring if you use a different topology where ON corresponds to a LOW
GPIO level.

The display (screen + backlight together) follows the night-hours schedule and
the long-press toggle; a day/night transition clears a manual override so the
schedule resumes control.

## Build
```bash
pio run              # build firmware
pio run -t upload    # flash firmware
pio run -t uploadfs  # flash LittleFS image (web pages in data/)
```
The web pages (config/log) live in `data/` and are served from LittleFS, so run
`uploadfs` at least once (and again whenever the HTML/CSS changes).

On Windows/WSL, symlink `.pio/build` to a native (non-`/mnt/c`) path to avoid cross-compiler errors.

## Configure
First boot opens a `miniDash-Setup` AP (pw `minidashpass`) for WiFi + location.
After connected, the device serves a config page at its IP (port 80) with a live
terminal panel. It also advertises itself over mDNS, so it's reachable at
**http://&lt;hostname&gt;.local** (default `minidash.local`; the hostname also
appears in the router's DHCP list). Configurable fields:

- WiFi SSID / password
- **Hostname** (mDNS/DHCP name, sanitized to valid DNS chars)
- Latitude / longitude, timezone (DST-aware dropdown)
- Weather refresh interval
- Metrics toggle
- **Night start / end hours** (screen off window; wraps midnight)
- **NTP resync interval** (minutes)
- **ESPHome host** and **sensors** as `slug=label` pairs, comma-separated
  (e.g. `living_temp=Living,humidity=Humidity`)
- Monitor list (HTTP reachability probes)
- **Flight radar range** (nm, 0 disables the screen)
- **Enabled screens** (per-screen checkboxes)
- **Backlight**: enable D6 transistor control + active-high polarity

Saving reboots the device.

Config is stored in LittleFS as `/config.json` (ArduinoJson). Legacy EEPROM
config is imported automatically once on first boot after upgrade.

## OTA updates
An OTA web updater is mounted at `http://<device-ip>/update` (or
`http://<hostname>.local/update`), and the config page
has an upload form. Both **firmware** (`firmware.bin`) and **filesystem**
(`littlefs.bin`) images are accepted and auto-detected. Build the images with
`pio run` / `pio run -t buildfs` and upload the `.pio/build/<env>/*.bin` files.

## Data sources
- Time: NTP (TZ from config), periodic resync at the configured interval with a
  green/red sync dot on the clock screen
- Weather: Open-Meteo (no API key) — current + 3-day forecast (labeled by weekday)
- External IP: ipinfo.io
- ESPHome: REST API at `http://<host>/sensor/<slug>` (enable `api: rest: true` in the ESPHome device YAML)
- Monitors: HTTP reachability probes
- Flights: adsb.fi open data API over TLS (`opendata.adsb.fi`), ~15s refresh

## Notes
- All network I/O is non-blocking (a shared HTTP state machine) so the clock keeps ticking.
- A hardware watchdog (~8s) auto-resets the device if the main loop freezes.
- WiFi is auto-supervised and reconnects; an "offline" marker shows when down.
- Free heap and fragmentation are shown on the System screen and logged periodically.
- Live serial log is viewable at `/log` (and on the config page) on the device web UI.
