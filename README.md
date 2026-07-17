# miniDash

ESP8266 (Wemos D1 mini) + 1.8" ST7735 TFT dashboard firmware.

## Screens
Button cycles: Clock → ESPHome → 3-Day Forecast → Network → Weather Detail → Monitors.

Screen turns off automatically during configurable night hours (default 23:00–07:00).

## Wiring
| Signal | Pin |
|--------|-----|
| CS     | D1 (GPIO5) |
| DC     | D2 (GPIO4) |
| RST    | D4 (GPIO2) |
| SCK    | D5 (GPIO14) |
| MOSI   | D7 (GPIO13) |
| Button | D3 (GPIO0), pull-up, falls to cycle screens |

Backlight is hardwired to 3.3V (not software-controllable).

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
terminal panel. Configurable fields:

- WiFi SSID / password
- Latitude / longitude, timezone (DST-aware dropdown)
- Weather refresh interval
- Metrics toggle
- **Night start / end hours** (screen off window; wraps midnight)
- **NTP resync interval** (minutes)
- **ESPHome host** and **sensors** as `slug=label` pairs, comma-separated
  (e.g. `living_temp=Living,humidity=Humidity`)
- Monitor list (HTTP reachability probes)

Saving reboots the device.

Config is stored in LittleFS as `/config.json` (ArduinoJson). Legacy EEPROM
config is imported automatically once on first boot after upgrade.

## OTA updates
An OTA web updater is mounted at `http://<device-ip>/update`, and the config page
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

## Notes
- All network I/O is non-blocking (a shared HTTP state machine) so the clock keeps ticking.
- A hardware watchdog (~8s) auto-resets the device if the main loop freezes.
- WiFi is auto-supervised and reconnects; an "offline" marker shows when down.
- Free heap and fragmentation are shown on the Network screen and logged periodically.
- Live serial log is viewable at `/log` (and on the config page) on the device web UI.
