# rgbcube-esp-bridge

> Note: AI was used for the majority of the code in this project.

ESP8266 firmware (Arduino + PlatformIO) to bridge WLAN TCP to STM32 UART.

## Wiring (runtime, fixed by your board)

- STM `PA09 (TX)` -> ESP `GPIO3 (RX)`
- STM `PA10 (RX)` <- ESP `GPIO2 (TX)`

## Important: Flash UART vs Runtime UART

Runtime bridge uses `GPIO3/GPIO2`, but ESP8266 flashing via `esptool` always uses UART0:

- ESP `GPIO3` = `U0RXD` (flash RX)
- ESP `GPIO1` = `U0TXD` (flash TX)

So for flashing, connect your USB-UART adapter to `GPIO1/GPIO3` (not `GPIO2`).

## ESP boot straps for flashing

- `GPIO0` -> LOW (GND)
- `GPIO2` -> HIGH
- `GPIO15` -> LOW
- toggle `EN/RESET`

## Config

1. Create local bridge secrets file:
   - copy `include/bridge_secrets.h.example` to `include/bridge_secrets.h`
   - set `BRIDGE_WIFI_SSID` and `BRIDGE_WIFI_PASS` in `include/bridge_secrets.h`
   - optional: set `BRIDGE_MQTT_*` and `BRIDGE_ESPHOME_*`
   - optional OTA: `BRIDGE_OTA_PORT` (default `8266`) and `BRIDGE_OTA_PASSWORD`
   - `include/bridge_secrets.h` is gitignored and will not be committed
2. Optional build settings in `platformio.ini`:
   - `BRIDGE_HOSTNAME` (default `rgbcube`)
   - `BRIDGE_TCP_PORT` (default `7777`)
   - `BRIDGE_UART_BAUD` (default `57600`)

## Build/Flash

Important: First disable the UART from STM32 to ESP.
Connect the Cube's small board to USB and open a terminal on the appearing port.
Disable the uart by entering `ue 0` and `Enter`.
You should see a message, that UART is disabled.

Then:
```bash
pio run
pio run -t upload
```

## OTA Upload (Wi-Fi)

After one serial flash, OTA is available while the bridge is connected to Wi-Fi.

```bash
pio run -e esp12e -t upload --upload-port rgbcube
```

If `BRIDGE_OTA_PASSWORD` is set, pass it during upload:

```bash
pio run -e esp12e -t upload --upload-port rgbcube --upload-flags="--auth=YOUR_OTA_PASSWORD"
```

## Flashing ESP on shared STM/ESP board

STM and ESP share UART lines on the same PCB. Disable the STM ESP-UART bridge first via STM CDC (USB-C), then flash the ESP.

1. Connect the board USB-C (STM32 CDC serial).
2. Open STM monitor (from `RGBCube888_STM32`):

```bash
pio device monitor -b 115200
```

3. In the monitor, send:

```text
eu 0
```

Expected response: `espUart=OFF` and log line `[ESP-UART] bridge disabled (pins high-Z)`.

4. Flash ESP firmware (this project) using ESP UART0 (`GPIO1/GPIO3`) as described above:

```bash
pio run -t upload
```

5. Re-enable STM ESP-UART bridge after upload:

```text
eu 1
```

## Use

- Connect to the same Wi-Fi as the ESP.
- Open TCP connection to `rgbcube:7777` (or ESP IP).
- Send STM CLI commands ending with `\n`, e.g.:

```bash
printf 'p\n' | nc rgbcube 7777
```

## Runtime bridge control (from STM SD config or serial)

The firmware now accepts bridge control lines via STM UART and USB serial:

- `wifi set "SSID" "PASS"`
- `wifi apply`
- `mqtt set host=... port=1883 user=... pass=... prefix=... client=... enabled=1`
- `mqtt apply`
- `esphome set mode=1 node=rgbcube`
- `bridge status`
- `bridge diag`
- `BRIDGE_WIFI_SSID=...`
- `BRIDGE_WIFI_PASS=...`
- `BRIDGE_MQTT_ENABLED=0|1`
- `BRIDGE_MQTT_HOST=...`
- `BRIDGE_MQTT_PORT=...`
- `BRIDGE_MQTT_USER=...`
- `BRIDGE_MQTT_PASS=...`
- `BRIDGE_MQTT_PREFIX=...`
- `BRIDGE_MQTT_CLIENT_ID=...`
- `BRIDGE_ESPHOME_MODE=0|1`
- `BRIDGE_ESPHOME_NODE=...`

On ESP boot, the bridge automatically requests config from STM with `sd bridge`
and applies incoming `BRIDGE_*`/`wifi set`/`mqtt set` lines before normal Wi-Fi
and MQTT startup.

MQTT behavior:

- Non-ESPHome mode topics:
  - command: `<prefix>/bridge/command`
  - state: `<prefix>/bridge/state`
  - availability: `<prefix>/bridge/availability`
  - STM display switch command: `<prefix>/bridge/stm_display/set`
  - STM display switch state: `<prefix>/bridge/stm_display/state`
  - STM mode select command: `<prefix>/bridge/stm_mode/set` (`0..10`)
  - STM mode select state: `<prefix>/bridge/stm_mode/state`
- ESPHome-like mode topics:
  - command: `esphome/<node>/command`
  - state: `esphome/<node>/state`
  - availability: `esphome/<node>/status`
  - STM display switch command: `esphome/<node>/stm_display/set`
  - STM display switch state: `esphome/<node>/stm_display/state`
  - STM mode select command: `esphome/<node>/stm_mode/set` (`0..10`)
  - STM mode select state: `esphome/<node>/stm_mode/state`
  - retained discovery topic: `esphome/discover/<node>-<mac6>`
  - retained Home Assistant configs:
    - `homeassistant/button/<node>-restart/config`
    - `homeassistant/button/<node>-stm_restart/config`
    - `homeassistant/switch/<node>-stm_display/config`
    - `homeassistant/select/<node>-stm_mode/config`
    - `homeassistant/sensor/<node>-bridge_state/config`

MQTT commands (all possible payloads):

- Main command topic (`.../command`):
  - `restart` -> restarts ESP bridge.
  - JSON-like restart payload containing both `"cmd"` and `restart` (for example `{"cmd":"restart"}`) -> restarts ESP bridge.
  - any other non-empty payload -> sent to STM UART with trailing `\n`.
  - in ESPHome-like mode only: `ON` maps to `m 1`, `OFF` maps to `m 2` before UART send.
- STM display switch command topic (`.../stm_display/set`):
  - ON values: `1`, `true`, `on`, `yes`, `enabled` -> sends `dp 1`.
  - OFF values: `0`, `false`, `off`, `no`, `disabled` -> sends `dp 0`.
  - toggle values: `toggle`, `t` -> sends `dp t`.
  - any other payload is ignored.
- STM mode select command topic (`.../stm_mode/set`):
  - integer payload `0..10` -> sends `m <n>`.
  - any other payload is ignored.

Notes:

- The Home Assistant discovery payloads are retained and include:
  - bridge restart button (`pl_prs={"cmd":"restart"}` on main command topic)
  - STM restart button (`pl_prs="rst"` on main command topic, forwarded as UART `rst\n`)
  - STM display switch (`pl_on="ON"`, `pl_off="OFF"` on `.../stm_display/set`)
  - STM mode select (`0..10` on `.../stm_mode/set`)
  - bridge state diagnostic sensor (reads `.../state`)

## STM side note

Set STM bridge UART baud to match `BRIDGE_UART_BAUD`.
