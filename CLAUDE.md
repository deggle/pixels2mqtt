# pixels2mqtt: project notes for Claude

## Goal
ESP32-C6 gateway for Pixels electronic dice. It listens to the dice's BLE adverts and never connects. It shows live state on a web dashboard, and publishes rolls, roll totals and battery to MQTT, with optional Home Assistant discovery.

## Hardware and toolchain
- Hardware: M5Stack NanoC6 (ESP32-C6), USB-C. The user has a full set of 7 Pixels dice.
- Build: PlatformIO in VS Code, Arduino framework. Platform is pioarduino `55.03.312-1` (Arduino core 3.x), pinned by URL in `platformio.ini`. The official `espressif32` platform ships Arduino core 2.x, which has no C6 support.
- Board: `esp32-c6-devkitm-1` with `board_build.variant = m5stack_nanoc6` (the core's NanoC6 pin map). pioarduino has no NanoC6 board profile.
- `-DARDUINO_USB_MODE=1` is required alongside `ARDUINO_USB_CDC_ON_BOOT=1`. The C6 only has USB-Serial/JTAG (HWCDC); without the flag, `Serial` maps to the S2/S3-only `USBSerial`.
- The NanoC6 is COM9 (VID:PID 303A:1001), pinned in `platformio.ini`. COM8 on the same machine is a separate classic ESP32 (CH340). Don't flash it.
- Partitions: `min_spiffs.csv` (1.9 MB app, OTA possible). Flash is 82.5% used. If it gets tight, use a 3 MB app partition and lose OTA.
- Downloads from the PlatformIO mirror sometimes fail with SSL errors: rerun. If `pio run` fails with `MissingPackageManifestError`, a download was cut off. Delete the package folder in `~/.platformio/packages` that has no `package.json`, then rebuild.
- Use `pio pkg ...`, not the deprecated `pio platform ...`.
- Serial monitor holding COM9 makes uploads fail with "Access is denied": close the monitor first.

## Architecture (`src/`)
- `main.cpp`: setup/loop and the button.
- `ble.*`: NimBLE advert listener (never connects).
- `dice.*`: per-die state table (locked, shared between the BLE task and readers), value rules, roll groups.
- `led.*`: status LED.
- `net.*`: Wi-Fi station or setup AP, DNS, mDNS.
- `web.*`: settings page, dashboard, `/ws` WebSocket.
- `mqtt.*`: publishing and HA discovery.
- `settings.*`: NVS (`Preferences`, namespace `p2m`).
- Rule: nothing blocks the NimBLE host task. Its callbacks only update the dice table and queue log events, which `bleLoop()` prints. A stalled host makes the controller discard adverts, which lost rolls when all the dice were rolled at once.
- `Serial.setTxTimeoutMs(0)`: with USB plugged in but no monitor reading, writes would otherwise block.

## Pixels protocol
- Spec: https://github.com/GameWithPixels/.github/blob/main/doc/CommunicationsProtocol.md. Message list: https://github.com/GameWithPixels/DiceFirmware (`src/bluetooth/bluetooth_messages.h`).
- Pixels service UUID `6e400001-b5a3-f393-e0a9-e50e24dcca9e`. Characteristics (verified): notify `6e400001-…` (the same as the service UUID), write `6e400002-…`.
- Advert layout (verified on all 7 dice):
  - Manufacturer data: company ID `FF FF`, ledCount, design (high nibble = die type, low nibble = colourway), rollState, currentFace, battery (bit 7 = charging, bits 0-6 = %). Example d00: `FF FF 0A 53 05 00 64`.
  - Service data `0x180a`, in the scan response only: pixelId (4, LE), buildTimestamp (4).
- RollState: 0 unknown, 1 rolled, 2 handling, 3 rolling, 4 crooked, 5 onFace. `face` is a 0-based index.
- Die types: 1 d4, 2 d6, 3 d8, 4 d10, 5 d00, 6 d12, 7 d20. The d4 has 6 LEDs, so never infer the type from the LED count.
- Values, from the user's rules; `dieFaceValue()` in `dice.cpp` is the only place for this:
  - d10: index, with 0 = 10.
  - d00: index × 10, with 0 = 100.
  - All others: index + 1.
- Connection messages, for future actions such as Blink (little endian, one-byte ID first): 1 WhoAreYou, 2 IAmADie (22 bytes: ledCount @1, colorway @2, dieType @3, pixelId @8, rollState @18, face @19, battery @20, batteryState @21), 3 RollState, 34 BatteryLevel, 29 Blink.

## Key decisions and why
- **Connectionless (user decision).** The core's prebuilt NimBLE porting layer has a fixed callout pool sized for 3 connections. NimBLE-Arduino uses one callout per `NimBLEClient`, so creating the 3rd client asserts in `npl_freertos_callout_init` and reboots. A shadow `sdkconfig.h` can't resize prebuilt pools. Adverts carry everything we need. Holding connections would require rebuilding the core libs with pioarduino `custom_sdkconfig`.
- **Scan callbacks.** With active scanning, NimBLE's `onResult` fires only after a scan response, and the controller backs off scan requests, which gave ~10 s updates. We handle `onDiscovered` (fires on every advert) as well as `onResult`. Active scanning stays on, so scan responses still deliver the name and pixel ID. Duplicates are allowed, the scan duty is 100%, and `setMaxResults(0)` is used.
- **Measured rate:** about 1.5 adverts per second per die, so a result appears up to ~0.7 s after the die settles. The handler takes under 1 ms.
- **No BLE in setup mode.** A continuous scan starves the soft AP: phones join but the portal won't load.
- **Wi-Fi sleep off** (`WiFi.setSleep(false)`). Modem sleep added up to ~2 s of lag. WebSocket Nagle is off too.
- **The router rejects the first Wi-Fi join (AUTH_FAIL).** The connect timeout is 30 s before falling back to the portal.
- **MQTT uses ESP-IDF's esp-mqtt**, publishing from its own task (`mqttPub`) with `esp_mqtt_client_publish`. Don't use `esp_mqtt_client_enqueue`: queued messages go out one per esp-mqtt poll pass (1 s, compiled into the prebuilt lib), which lagged.
- **ESPAsyncWebServer rather than WiFiManager.** One async server covers the portal, settings and WebSocket UI.

## Behaviour
- **Online:** an advert heard within 15 s. Otherwise the die is shown as asleep.
- **Roll group (total):**
  - It starts when a die starts rolling with no group open; dice that start rolling while it's open join it.
  - It ends when no member is rolling. The total is the sum of the members' landed values (rolled or on face).
  - Crooked, or caught mid-tumble, means no value.
  - The logic is in `dice.cpp` (`RollGroup`).
- **MQTT topics** (plain integer payloads):
  - `pixels/<die>/rolled`, not retained.
  - `pixels/<die>/rolling`: 1/0.
  - `pixels/<die>/battery`, retained.
  - `pixels/total`.
  - `pixels/status`: online/offline, retained, used as the last will.
  - `<die>` is the die type (d6), or the pixel ID if "Use UID in topics" is on. In type mode, duplicate types share a topic, by the user's choice. There is no `/id` topic.
- **HA discovery** (setting):
  - Per die, a device `pixels_<uid>` with Roll sensor (force_update), Roll event, Rolling binary sensor and Battery. Event entities are for automations, because they fire on repeat values.
  - The gateway device `pixels2mqtt_<mac>` has Roll total and Total event.
  - Configs are re-sent on connect and when `homeassistant/status` = online. Turning discovery off doesn't remove the retained configs.
- **Status LED** (WS2812, data GPIO20, power GPIO19): blue while any die rolls, green flash on a result, red flash if crooked, dim magenta in setup mode.
- **Setup mode:**
  - Open AP `pixels2mqtt-setup`, portal at 192.168.4.1.
  - Entered when there's no SSID or the Wi-Fi join fails.
  - With saved credentials, it reboots to retry after 5 minutes idle.
- **Button** (GPIO9):
  - A short press reboots. A 5 s hold clears settings: dim red after 1 s, red blink once cleared.
  - Both act on release, because GPIO9 is the boot strapping pin.
  - Track "pressed" with its own flag. A `now | 1` timestamp trick made `held` wrap on even millis and wiped settings on short presses (fixed, simulated).

## State
- Repo on `main`, remote https://github.com/deggle/pixels2mqtt (public). Commit and push only when the user asks.
- Everything above is working on hardware with all 7 dice.

## Next steps
1. Web UI actions, e.g. Blink to identify a die. Needs a brief on-demand connection, one at a time.
2. MQTT over TLS, if a remote broker is ever used.
3. Optional: clean up the HA discovery configs when discovery is turned off.

## Working rules
- The user is in Sheffield, UK. Use British English, metric units, and a clear technical tone.
- Explain serial output and hardware steps plainly. The user is comfortable with coding and electronics.
- Keep changes small and test each on hardware before adding the next feature.
- Keep personal infrastructure (broker hostnames, LAN IPs, SSIDs) out of committed files: the repo is public.
