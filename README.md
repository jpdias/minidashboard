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
| Backlight ctrl | D8 (GPIO15), optional transistor gate/base |

```
        Wemos D1 mini                        ST7735 1.8" TFT
       +--------------+                      +---------------+
       |          3V3 |--------------------->| VCC           |
       |          3V3 |--------------------->| LED+ (anode)  |
       |   D1 (GPIO5) |--------------------->| CS            |
       |   D2 (GPIO4) |--------------------->| DC / A0       |
       |   D4 (GPIO2) |--------------------->| RES / RST     |
       |  D5 (GPIO14) |--------------------->| SCK / CLK     |
       |  D7 (GPIO13) |--------------------->| SDA / MOSI    |
       |              |                      | GND (common)  |---+  (cut this trace
       |   D3 (GPIO0) |---+                  +---------------+   |   to Wemos GND;
       |  D8 (GPIO15) |-+ |                                      |   route via NPN)
       |          GND |-|-|--------+                             |
       +--------------+ | |        |                             |
                        | |        |                             |
                        | |  [ Button ]                          |
                        | +----o  o----+   (short=cycle screen,  |
                        |              |    long=display on/off)  |
                        |             GND                         |
                        |                                         |
                        |   Backlight/power control (optional)    |
                        |          BC547 NPN                       |
                        |            .---.                         |
                        |  base     /  C  \  collector            |
                        +--[2k2]---|B      |------------------------+  (panel GND)
                                    \  E  /
                                     '-|-'
                                       |  emitter
                                      GND  (Wemos GND)
```
> **Note:** the BC547 is a **low-side switch on the panel's *common GND***, not
> just the LED line. GPIO15 HIGH turns the transistor on and connects the panel's
> GND, powering the *whole* ST7735 (logic **and** backlight) — active-high. Cutting
> GND therefore fully powers down the panel, so waking it is a cold boot (restore
> power first, then re-init the display). See below for details and alternative
> transistor topologies.

### Backlight control (optional)
By default the ST7735 is always on. This build switches the **whole panel** in
software by putting a transistor in its **common GND** line, driven by
**D8 (GPIO15)**. (You can instead switch only the LED- line for backlight-only
control — see the topology table below.)

> **Pin choice matters.** Avoid **D6/GPIO12** (it's the hardware-SPI MISO line;
> the TFT's SPI init reclaims it, so it can't hold a level) and **D0/GPIO16**
> (a special I/O pin that won't drive a transistor cleanly). **D8/GPIO15** is a
> normal, fully-controllable GPIO. It has a boot-strap pulldown, so the backlight
> stays off for the first moment of boot and then the firmware drives it HIGH —
> this also means the transistor base must not be pulled high externally.

**This build uses a BC547 (NPN) as a low-side switch on the GND line** — the
simplest option and the one wired here:

- Cut the panel's **common GND** trace to the Wemos GND.
- **Collector** → panel GND (the side you just cut).
- **Emitter** → GND.
- **Base** → **D8 (GPIO15)** through a ~1kΩ resistor.
- This is **active-high**: GPIO HIGH turns the panel ON.

> **Tip — dimming:** a larger base resistor partially starves the transistor so it
> doesn't saturate, dropping a little more voltage across it and **dimming the
> backlight**. This build uses **2.2kΩ** for a slightly dimmer, easier-on-the-eyes
> panel. Use ~1kΩ for full brightness, or go higher for dimmer (too high and the
> panel may flicker or not power up reliably).

There are plenty of other ways to do this depending on the parts you have and
whether you want a high-side (3.3V line) or low-side (GND line) switch:

| Type | Wiring | Cut | Active level |
|------|--------|-----|--------------|
| **NPN (BC547, 2N2222)** — used here | Collector→panel GND, Emitter→GND, Base→D8 via 2.2kΩ (1kΩ=full brightness, higher=dimmer) | common GND→Wemos GND | HIGH |
| N-MOSFET (2N7000, AO3400) | Drain→LED−, Source→GND, Gate→D8 (100Ω series, 100k gate→GND) | LED cathode→GND | HIGH |
| PNP (2N2907, BC557) | Emitter→3.3V, Collector→LED+, Base via 10k pull-up + NPN driver | LED anode→3.3V | depends on driver |
| P-MOSFET + NPN | P-FET in 3.3V line, gate driven by small NPN from D8 | LED anode→3.3V | depends on driver |

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
- **Backlight**: enable D8 transistor control + active-high polarity

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
- Sun/moon: USNO Astronomical Applications API over TLS (`aa.usno.navy.mil`) —
  sunrise/set, moonrise/set, moon phase and illumination. Fetched **synchronously at
  boot** (the first TLS session, running alone) and then once more per local day via
  the non-blocking FSM. Because the boot fetch blocks and runs before the flight
  radar's TLS session, the two never collide — this fixed the intermittent
  "moon sometimes doesn't load" race.
- Forecast: Open-Meteo, fetched at boot and then **twice a day** (midnight + noon, local).

## Notes
- All network I/O is non-blocking (a shared HTTP state machine) so the clock keeps ticking.
- A hardware watchdog (~8s) auto-resets the device if the main loop freezes.
- WiFi is auto-supervised and reconnects; an "offline" marker shows when down.
- Free heap and fragmentation are shown on the System screen and logged periodically.
- Live serial log is viewable at `/log` (and on the config page) on the device web UI.

### Boot sequence (deterministic one-time fetch)
Instead of racing several non-blocking FSMs at startup, `setup()` runs a fixed,
**blocking** boot sequence with a live on-screen step list (`miniDash` → `Fetching
data...` → one line per step, `+`=done / `!`=fail / `.`=working):
1. **NTP sync** — wait for a real clock (date-based fetches are meaningless at epoch).
2. **Weather** + **Forecast** + **External IP** — plain HTTP, no TLS contention.
3. **Sun / Moon** — TLS to USNO; runs *alone* as the first TLS session.
4. **Flight radar** — TLS to adsb.fi; runs only after moon released the lock.
5. **Ready** — hand off to the main loop.

Each step either completes or times out (12s) before the next starts, so there's no
overlap of the two TLS sessions and the radar/moon can never wedge as they used to.
The forecast is then refreshed only **twice a day** (midnight + noon) instead of on
every weather cycle.

## Troubleshooting (problems hit during development)

These are real issues encountered building this firmware and how they were solved.
Kept here so the same wall isn't hit twice.

### Two TLS connections at once exhaust the heap
The ESP8266 heap can rarely fit **two concurrent BearSSL sessions**. When the moon
fetch (`aa.usno.navy.mil`) and the flight radar (`opendata.adsb.fi`) tried to run
at the same time, one would fail with `NoMemory`, and the flight FSM could wedge —
symptom: **radar does its first fetch, then the refresh countdown sticks at 0 and
never refreshes again.**
- **Fix:** a shared single-session lock (`tlslock.h/.cpp`, global `gBusy`). Each
  fetcher calls `tls_try_acquire()` before `new WiFiClientSecure` and
  `tls_release()` in cleanup, so only one TLS session exists at a time.
- **Belt-and-braces:** the lock **self-releases after 20s** in case a holder
  crashes or forgets, and the flight FSM has a **12s global phase watchdog** that
  aborts the cycle if any phase stalls — so a stuck countdown can no longer happen.
- Also reduce TLS buffers (`setBufferSizes(4096, 512)`) and gate fetch starts on
  `ESP.getMaxFreeBlockSize()` so a fetch only begins when contiguous heap is available.

### Radar SSL is intentionally *not* validated
Cert validation is off (`setInsecure()`). If the radar freezes it is **not** an SSL
handshake problem — look at the FSM/lock (above), not certificates.

### Backlight transistor pin choice
Getting software backlight control working took three pins:
- **D6/GPIO12** does not work — it's the hardware-SPI **MISO** line; the TFT SPI
  init reclaims it so it can't hold a level.
- **D0/GPIO16** does not work — special I/O pin, won't drive a transistor cleanly.
- **D8/GPIO15** works — normal GPIO. Note its boot-strap pulldown: the base must
  not be pulled high externally.

### Cutting GND kills the whole panel (cold boot on wake)
The BC547 low-side switch cuts the ST7735's **common GND**, which removes power from
the *entire* panel (logic + backlight), not just the LED. So waking the display is a
**cold boot**: `backlight_write(true)` must run **first** to restore power, then a
50ms settle, then `initR()` + `setRotation(2)` + a full redraw. Doing `ui_poweron()`
before restoring power initializes a dead panel and shows nothing.

### `monitors` silently dropped from config
Extra monitor hosts didn't persist. The ArduinoJson document was too small (2048);
on overflow it **silently dropped the last array** (`monitors`). **Fix:** raised the
config doc to 4096 and added an overflow warning + byte-count log.

### Save page hung the browser tab
After saving, the browser tab would hang waiting on a socket the device closed on
reboot. **Fix:** the save response sends `Connection: close`, flushes and stops the
socket, then `ESP.restart()` after 200ms; `saved.html` shows a 10s JS countdown that
redirects back to `/`.

### Moon fetched with the wrong (epoch) date at boot
Before NTP sync, `time()` returns 1970, so the moon fetch used the wrong date.
**Fix:** gate the fetch behind `time_is_synced()` (plus an ~8s boot grace) so it only
runs once the clock is real. The timezone offset is computed via a
`mktime(gmtime(now))` trick because this newlib build lacks `tm_gmtoff`.

### Text rendered double-size unexpectedly
Adafruit GFX text size is **sticky** — after drawing the temperature at
`setTextSize(2)`, the sun/moon rows inherited size 2. **Fix:** always reset
`setTextSize(1)` before the small rows. (Also: the built-in font is 7-bit ASCII
only — no Unicode glyphs, so moon phase is drawn as a custom filled shape.)

### WSL can't flash over serial
The upload (`pio run -t upload` / `-t uploadfs`) must run from **Windows** — WSL
can't drive the serial `/dev/ttyS*` ports. Building works fine in WSL. Also symlink
`.pio/build` to a native (non-`/mnt/c`) path to avoid cross-compiler path errors.
