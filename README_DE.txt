# ESP32 Energy Monitor V1.0 – Arduino IDE

## Hardware basis
Target: common ESP32-2432S028 / ESP32-2432S028R CYD, 2.8" ILI9341, XPT2046 resistive touch, microSD.
This is a starter V1.0 implementation. CYD clones/revisions may differ; verify the controller and pins before relying on it.

## Included
- HTTP GET JSON reader polling (default every 1 second)
- Manual host/port/path and JSON-path configuration
- mDNS HTTP service discovery endpoint (`/api/discover`)
- Live web dashboard and JSON status API
- microSD daily CSV files; one row per minute; 365-day retention pruning
- Basic TFT live display and touch navigation
- Wi-Fi setup AP if saved WLAN credentials are missing/unavailable

## Arduino IDE setup
1. Install ESP32 by Espressif Systems using Boards Manager. Use an ESP32 core 2.x release for this initial sketch.
2. Install libraries:
   - TFT_eSPI (Bodmer)
   - XPT2046_Touchscreen (Paul Stoffregen)
   - ArduinoJson (Benoit Blanchon), version 7.x
3. Open the `ESP32_Energy_Monitor.ino` file.
4. Configure TFT_eSPI for the CYD before compiling:
   - In `TFT_eSPI/User_Setup_Select.h`, select a setup configured for ILI9341.
   - Pins: MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, RST=-1, BL=21.
   - Use `ILI9341_2_DRIVER` if required by your TFT_eSPI version/board.
   - Display size is 240x320 portrait.
5. Select `ESP32 Dev Module`, 4 MB flash, upload at 115200 if upload is unreliable.
6. Insert a FAT32-formatted microSD card.
7. Compile and upload.

## First start
- If Wi-Fi cannot connect, the ESP32 starts AP `EnergyMonitor-Setup` with password `energy1234`.
- Connect to that AP and open `http://192.168.4.1/config`.
- Enter home Wi-Fi SSID/password and reader settings.
- After reboot, open `http://energy-monitor.local` or the ESP32's IP address.
- Settings are stored in NVS/Preferences.

## Reader JSON mapping
The configuration page accepts dot-separated JSON paths. Example:
```json
{
  "1.8.0": 12345.678,
  "2.8.0": 3456.789,
  "1.7.0": 820,
  "2.7.0": 0
}
```
Set:
- import counter path: `1.8.0`
- export counter path: `2.8.0`
- import power path: `1.7.0`
- export power path: `2.7.0`

For a signed `16.7.0` power field, enable "Vorzeichenbehaftete Leistung verwenden", set the signed path to `16.7.0`, and select the sign convention.

Nested JSON is supported, e.g. `meter.energy.import`. Array indexes are supported, e.g. `meters.0.value`.

## Discovery
`/api/discover` queries mDNS for services advertising `_http._tcp`. This is intentionally non-invasive. Not every smart-meter reader advertises mDNS; such devices must be entered manually. Broad subnet port scanning is not enabled in this starter build.

## Data storage
Daily files are written as `/YYYY-MM-DD.csv`, with one row per minute:
timestamp, import_kwh, export_kwh, avg_import_w, avg_export_w, max_import_w, max_export_w

The code retains approximately 365 days based on daily filenames. NTP time must be available for correct timestamps and retention.

## Important limitations before unattended use
- This is a starter firmware, not yet a field-validated appliance. Test it with your exact CYD revision, SD card, and reader.
- Touch calibration constants are generic and likely need adjustment.
- The display touch screen currently offers basic navigation; full configuration is via browser.
- The web server is plain HTTP and intended for a trusted local LAN only. Do not expose it directly to the internet.
- Reader polling is synchronous HTTP. A slow reader can delay the main loop; configure reader response time below the polling interval for true 1 Hz operation.
- If only `16.7.0` is provided, import/export split depends on the meter's sign convention. Prefer separate 1.7.0 and 2.7.0 fields when available.
- The example does not yet provide historical chart rendering; CSV download is available via `/api/history?date=YYYY-MM-DD`. Add chart UI in a subsequent revision.
- For robust deployment, test SD card endurance, power-loss behavior, time synchronization, and daily file pruning.
