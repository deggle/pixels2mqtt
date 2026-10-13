# pixels2mqtt

A small ESP32-C6 gateway for [Pixels](https://gamewithpixels.com/) electronic dice. It listens to the dice over Bluetooth Low Energy, shows every roll live in a web dashboard, and publishes rolls, roll totals and battery levels to MQTT, with optional Home Assistant discovery.

It never connects to the dice. Pixels broadcast their die type, roll state, face and battery in their BLE adverts, so the gateway just listens. That means there's no connection limit, any number of dice can be followed, and it uses no extra battery on the dice.

## Features

- **Any Pixels die in range** is picked up automatically: d4, d6, d8, d10, d00, d12 and d20.
- **Live dashboard:** a card per die with value, state, battery and signal strength, updated over a WebSocket.
- **Roll totals:** dice rolled together are grouped, and their results summed once the last one stops.
- **MQTT** with plain integer payloads, so it's easy to use from Home Assistant, Node-RED or a shell.
- **Home Assistant discovery** (optional): each die appears as a device with Roll, Roll event, Rolling and Battery entities.
- **Status LED:** blue while dice are rolling, a green flash on a result, a red flash if a die lands crooked.
- **Setup portal:** on first boot it creates its own Wi-Fi network for setting up Wi-Fi and MQTT.

## Hardware

- [M5Stack NanoC6](https://docs.m5stack.com/en/core/M5NanoC6) (ESP32-C6). Its built-in RGB LED and button are used.
- One or more Pixels dice.

Other ESP32-C6 boards should work after changing the pins in `platformio.ini`, but only the NanoC6 has been tested.

## Build and flash

1. Install [VS Code](https://code.visualstudio.com/) with the PlatformIO IDE extension.
2. Open this folder. PlatformIO downloads the pioarduino platform (Arduino core 3.x, needed for the C6) on the first build.
3. Set `upload_port` / `monitor_port` in `platformio.ini` to your board's port, or remove them to auto-detect.
4. Build and upload: `pio run -t upload`. Monitor: `pio device monitor` (115200 baud).

If the first build fails partway through a download, run it again. A failed build that reports `MissingPackageManifestError` means a package download was cut off: delete that package's folder under `~/.platformio/packages` and rebuild.

## First-time setup

1. Power the board. The LED glows dim magenta: it's in setup mode.
2. Join the Wi-Fi network **`pixels2mqtt-setup`** from a phone or laptop. The setup page should open by itself; if not, browse to **http://192.168.4.1/**.
3. Choose your Wi-Fi network, then fill in the MQTT broker if you have one (leave the host blank to disable MQTT). Save.
4. The board reboots onto your network. Open **http://pixels2mqtt.local/** (or its IP address, shown in the serial log) for the dashboard.

Settings are kept in flash across reboots and firmware updates. If Wi-Fi can't be joined, the setup network comes back. With saved settings, it retries your Wi-Fi every 5 minutes.

## The button

| Action | Result |
|---|---|
| Press | Reboot |
| Hold 5 s | Clear all settings and reboot into setup mode. The LED turns dim red after 1 s as a warning, then blinks red once cleared. |

Both happen when you release the button.

## MQTT

Payloads are plain integers. `<die>` is the die type (`d20`), or the die's unique ID (`3D3A8B73`) if **Use UID in topics** is ticked in settings. Use UID mode if you have more than one die of the same type.

| Topic | Payload | Retained | Sent |
|---|---|---|---|
| `pixels/<die>/rolled` | the value rolled | no | each completed roll |
| `pixels/<die>/rolling` | `1` / `0` | no | when it starts / stops tumbling |
| `pixels/<die>/battery` | `0`–`100` | yes | on change |
| `pixels/total` | sum of the dice rolled together | no | when the last of them stops |
| `pixels/status` | `online` / `offline` | yes | gateway up; `offline` is the last will |

Example: `mosquitto_sub -h <broker> -t 'pixels/#' -v`

### Values

- **d10:** faces 1–9 as shown, and **0 reads as 10**.
- **d00 (percentile):** faces read ×10 (10–90), and **00 reads as 100**.
- All other dice read as their face value.

### Roll totals

A group starts when a die starts rolling. Any die that starts rolling before the group ends joins it. The group ends when none of its dice are still rolling. The total is the sum of the results. A die that lands crooked, or is caught mid-tumble, has no result and isn't counted.

## Home Assistant

Tick **Home Assistant discovery** in settings (MQTT must be set up in HA). Each die appears under *Settings → Devices → MQTT* as **Pixels d20** and so on, with:

- **Roll:** sensor showing the last value.
- **Roll event:** event entity. **Use this to trigger automations**: it fires on every roll, even the same number twice, which a sensor's state doesn't.
- **Rolling:** binary sensor.
- **Battery:** battery sensor.

The gateway device has **Roll total** and **Total event**. Everything shows as unavailable if the gateway goes offline. Unticking discovery later doesn't remove the devices from HA; delete them there.

## How it works

Pixels dice advertise continuously. Each advert's manufacturer data carries the LED count, the die type and colourway, the roll state, the face index and the battery level. The scan response adds the die's name and unique ID. The gateway scans continuously, decodes each advert into a per-die state table, and feeds the dashboard, the LED and MQTT from that table.

The protocol is documented by Pixels in their [communications protocol](https://github.com/GameWithPixels/.github/blob/main/doc/CommunicationsProtocol.md).

### Latency

Each die advertises roughly every 0.7 s, so a result reaches the dashboard and MQTT within about a second of the die settling, even with all seven dice rolled at once.

## Project layout

```
src/
  main.cpp      setup/loop, button
  ble.*         BLE advert listener
  dice.*        per-die state, value rules, roll groups
  led.*         status LED
  net.*         Wi-Fi station / setup access point, mDNS
  web.*         settings page and live dashboard
  mqtt.*        MQTT publishing, Home Assistant discovery
  settings.*    persisted settings
```
