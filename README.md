# miniTV

ESP8266 (Wemos D1 mini) + 1.8" ST7735 TFT dashboard firmware.

## Screens
Button cycles: Clock → ESPHome → 3-Day Forecast → Network → Weather Detail → Monitors.

Screen turns off automatically 23:00–07:00.

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
pio run            # PlatformIO
pio run -t upload  # flash
```
On Windows/WSL, symlink `.pio/build` to a native (non-`/mnt/c`) path to avoid cross-compiler errors.

## Configure
First boot opens a `miniTV-Setup` AP (pw `minitvpass`) for WiFi + location.
After connected, the device serves a config page at its IP (port 80): WiFi, lat/lon, timezone, weather refresh, metrics toggle, ESPHome host, and monitor list. Saving reboots.

Config is stored in EEPROM.

## Data sources
- Time: NTP (TZ from config)
- Weather: Open-Meteo (no API key) — current + 3-day forecast
- External IP: api.ipify.org
- ESPHome: REST API at `http://<host>/sensor/<id>` (enable `api: rest: true` in the ESPHome device YAML)
- Monitors: HTTP reachability probes

## Notes
- All network I/O is non-blocking (state machines) so the clock keeps ticking.
