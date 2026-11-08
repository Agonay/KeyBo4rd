# KeyBo4rd — AGENTS.md

ESP32 custom mechanical keyboard with BLE HID, 6×13 matrix, rotary encoder knob, OLED + E-Ink displays.

## Hardware pinout

| Component | Pin / Address | Purpose |
|---|---|---|
| **Matrix rows** (ESP32 GPIO, INPUT_PULLUP) | 25, 33, 12, 26, 27, 14 | Row scan lines |
| **Matrix cols** (PCF8575 I2C expander, addr 0x20) | P15 P13 P4 P1 P3 P7 P5 P6 P11 P0 P2 P14 P12 | Column drivers |
| **Rotary encoder CLK** | 18 (shared with E-Ink SCK) | Quadrature A |
| **Rotary encoder DT** | 19 | Quadrature B |
| **Rotary encoder SW** | 13 | Push button (active-LOW, PULLUP) |
| **E-Ink (GxEPD2 1.54" D67)** | SCK=18 (shared) MOSI=23 CS=5 BUSY=15 RES=17 DC=16 | Boot image / idle screens |
| **OLED (SSD1306 128×64)** | I2C 0x3C | Boot splash, home screen (BT, battery, WPM graph, caps, key count, uptime), menu |
| **I2C bus** | SDA=21 SCL=22 (default Wire) | OLED + PCF8575 |

## Matrix layout

```
Rows: 6 (ESP32 pins) × Cols: 13 (PCF8575 pins) = 78 physical positions
```

Row 0 = function keys (F1-F12 + Escape). Row 1 = number row. Rows 2-5 = QWERTY area.
The **Fn** key is row 5 col 6 (left of ←); it is handled on the keyboard and never sent to the host.

## Libraries

- **ESP32-BLE-Keyboard** — HID keyboard + media keys over BLE
- **PCF8575** — I2C GPIO expander
- **GxEPD2** — E-Ink (GxEPD2_154_D67)
- **Adafruit SSD1306 + GFX** — OLED
- **Wire** — I2C bus
- **Preferences** — NVS key/value persistence for settings

## Board / build

- ESP32 Dev Module (Arduino framework, 240 MHz)
- Build with Arduino IDE / PlatformIO (`platform = espressif32`)
- Flash at 921600 baud (recommended)

## Architecture

```
main.ino
  initBluetooth()      → BleKeyboard (device "K3YBo4rd")
  initKeyboardMatrix() → PCF8575 + row scan
  initDisplay()        → OLED + E-Ink boot
  initRotaryEncoder()  → knob + button

loop():
  scanKeyboardMatrix()  → drives columns LOW, reads rows, sends key events
  handleButtonPress()   → knob button FSM (short/long/very-long)
  handleRotaryEncoder() → drains ISR detents → volume or menu navigation
```

| File | Responsibility |
|---|---|
| `BluetoothHID.{h,cpp}` | BLE device + LED callback (CapsLock) |
| `KeyboardMatrix.{h,cpp}` | Matrix scanning, debounce, key repeat, HID dispatch |
| `KeyDefinitions.h` | Layout table (Key structs with normal/shift/alt + modifier flag) |
| `RotaryEncoder.{h,cpp}` | ISR quadrature decoder, button event FSM, menu/volume routing |
| `DisplayManager.{h,cpp}` | OLED status line, E-Ink idle screens + menu rendering |
| `MenuManager.{h,cpp}` | Menu state machine, settings model, NVS persistence |
| `HomeControl.{h,cpp}` | Fn layer → Home Assistant (REST over LAN HTTP, worker task, Wi-Fi on demand) |
| `BatteryManager.{h,cpp}` | Battery status (simulated; real ADC later) |
| `main.ino` | Wiring + event loop |

## Key data model

```cpp
struct Key {
  uint8_t normalKey;   // HID code or ASCII when no modifier
  uint8_t shiftKey;    // HID code / char when shift is held
  uint8_t altKey;      // HID code / char when AltGr (RIGHT_ALT) is held
  bool isModifier;     // true → press/release modifier directly
};
```

The OS is configured **Spanish (Spain) on ANSI physical**. Characters are produced by sending the HID physical code that matches a Spanish layout; the OS maps to the final character. Dead keys (`´`, `¨`) and AltGr (`@`, `€`, etc.) work via the existing modifier mechanism.

## Coding conventions

- **Never call BLE (press/write/release) inside an ISR.** ISRs set volatile flags; the loop drains them.
- **Guard all BLE sends** with `bleKeyboard.isConnected()`.
- **No `Serial.print` inside ISRs** — use only in loop/handlers.
- **E-Ink is slow** (~2 s full refresh, ~500 ms partial). Redraw only on meaningful change; use partial windows.
- **OLED** is fast; redraw on state change only (no per-frame refresh).
- `debounceDelay` for matrix is 100 ms; key repeat starts at 500 ms, repeats every 30 ms.
- Use `Preferences` for settings persistence — namespace `"kbd"`.

## Known issues / debugging tips

| Symptom | Likely cause | Fix |
|---|---|---|
| Key spams endlessly on Windows | BLE disconnected while key held → latched report; no release on reconnect. | Guard with `isConnected()`, call `releaseAll()` on reconnect edge, forced release after 2 s. |
| Lag builds up while typing, key sticks ~2 s then repeats, BLE drops | Bluedroid notifications don't block; reports sent faster than the link piled up inside the stack. | Reports go through a 16-entry queue in `KeyboardMatrix.cpp`, sent only when `bleCanSend()` (`esp_ble_get_cur_sendable_packets_num`). Serial logs `[BLE] heap/sendable/q` every 10 s. |
| Knob misses volume steps at fast rotation | ISR level-triggered (`LOW`), 1 ms debounce drops transitions, BLE called inside ISR. | ISR fires `CHANGE` on both pins, tiny filter, BLE in loop only. |
| Spanish chars wrong | `BleKeyboard.press(char)` uses US ASCII map; Spanish-only chars (`ñ ¿ ¡ º`) not in map. | Use Spanish(ES)-on-ANSI layout table with physical HID codes. |
| E-Ink ghosting / stutter | Full refresh every frame. | Use partial updates + dirty-cooldown. |
| `einkDisplay` linker error if commented code re-enabled | `DisplayManager.h:13` declares `einkDisplay` extern but `.cpp` defines `display`. | Renamed or fixed. |

### Serial debugging

- `Serial.begin(115200)` — all modules log here.
- Matrix prints row/col on press.
- Knob prints `ENC CW / CCW`.
- BLE prints `[BLE] CapsLock: ON/OFF`.
- Menu prints `[MENU] event=X`.

### Knob and menu controls

```
Home mode:         turn = adjust device   press = toggle device   hold = leave Home mode
Outside the menu:  turn = volume (spin fast = 2x/3x steps)
                   press = play/pause   double-press = mute
                   hold (0.8 s) = open menu   hold 2.5 s = releaseAll (stuck-key recovery)

Menu list:  turn = move   press = select / edit   hold = close
Editing:    turn right = +, turn left = -   press = save   hold = cancel (revert)
            The edited value shows as <value>. Auto-close after 20 s without input.

Items: Volume step (1-4), Direction, OLED bright (0-100%, live preview),
       Show battery, Low battery (5-50%), Idle screen (Totoro/Status/Animation),
       Check update (checks GitHub now, installs immediately if newer),
       About (firmware, Wi-Fi, IP, Bluetooth, uptime), < Exit
Header shows the firmware version (FW vN / FW dev).
```

- **OLED home screen** (`oledDrawStatusLine()`): status bar (BT state, battery), body (WPM + ~2 min history graph when connected, pairing animation otherwise), footer (CAPS badge, key presses since boot, uptime h:mm). Knob actions show a ~1 s toast (`homeToast` / `homeToastVolume`). Keys/WPM refresh at 1 Hz so typing doesn't stall the scan on I2C (~23 ms per frame).
- **Boot splash:** `initDisplay()` plays a ~0.9 s animation; `bootSplashStatus(text, pct)` updates it from `setup()` until BLE is up.

- Encoder: full-cycle (4 transitions per click, rests at A=B=1). A click counts when the knob returns to rest, re-syncing every click. `-DENC_HALF_STEP` for half-cycle encoders; `-DENC_DEBUG` logs raw ticks.

## Home Assistant (Fn layer)

- **Keys:** Fn tap = Home mode on/off. Fn+1..9 = pick device N (enters Home mode). Any key pressed while Fn is held is kept out of HID reports until released (`KeyState::fnConsumed`). Home mode ends on Fn tap, knob hold, or 60 s without input.
- **Knob in Home mode:** press = `homeassistant.toggle` (scene/script → `turn_on`, button → `press`, media_player → `media_play_pause`). Turn by domain: light → `light.turn_on` `brightness_step_pct` ±10/click, fan → `increase/decrease_speed` per click, media_player → `volume_up/down`, cover → open/close, input_number/counter → increment/decrement. Other domains have no dial action.
- **Displays:** E-Ink shows the device list (one full refresh on entering Home mode or changing device; the knob button is still polled during the refresh, rotation is not). OLED shows device name + live state (`GET /api/states/<entity>` after each action).
- **Config** (setup portal: hold the knob at power-on, `http://192.168.4.1`): NVS `kbd`/`haUrl` (must be `http://`; TLS doesn't fit in RAM next to Bluedroid), `haToken` (long-lived access token), `haSlots` (one device per line, line N = Fn+N: `Name = entity` or `Name = press_entity, turn_entity`).
- **Runtime:** HTTP runs in the `home` FreeRTOS task (6 KB stack, core 0) so the matrix scan never blocks. Knob clicks are coalesced while a request is in flight. Wi-Fi (STA + modem sleep for BLE coexistence) starts on first use and stops after 5 min without Home actions. Serial: `[HOME] ...` incl. heap at Wi-Fi start/up/off.

## Firmware updates (OTA from GitHub)

- **Release flow:** a push to `master` that touches `src/**`, `platformio.ini` or `scripts/**` runs `.github/workflows/firmware.yml`. Versions are `MAJOR.MINOR`: MAJOR is in `firmware-version.txt` (bump by hand), MINOR counts up per release. The integer code is `MAJOR*1000+MINOR` (6.1 = 6001), used for `FW_VERSION`, the `version.json` `version` field and the tag `fw-<code>` (the release is named `Firmware v6.1`). Old firmware (≤ v6) builds the download URL from that tag, so keep the tag format. It publishes `firmware.bin` and `version.json` (`{"version","md5","size","notes"}`; `notes` = commit title, shown on the update screen).
- **Device side (`OtaUpdater.{h,cpp}`):**
  - `otaBootCheck()` runs before `initBluetooth()`. It joins Wi-Fi, reads `releases/latest/download/version.json`, and if the release is newer, streams `firmware.bin` into the spare OTA slot (MD5-checked) and reboots.
  - Checks only run in the boot path. With BLE up there are ~60 KB free, Wi-Fi init leaves ~25 KB, and the TLS handshake fails ("X509 - Allocation of memory failed"); BLE memory can't be freed without a restart.
  - `otaLoop()`: after 4 min idle (no keys/knob, menu closed), at most once an hour, it shows a 10 s "Restarting in Ns" countdown (any key/knob postpones to the next idle window), stores `otaChk=1` in NVS and reboots into the boot check.
  - Menu "Check update" (`otaCheckNow()`) stores `otaChk=2` and reboots. The boot check then shows "Asking GitHub...", "UP TO DATE" or the failure reason, or installs. Allowed on dev builds.
  - The manifest fetch retries 3 times (GitHub sometimes returns 5xx).
  - Update screen: title bar, `vA -> vB`, release notes, "Downloading x/y KB" + progress bar, "Verifying...", then "UPDATE COMPLETE" or "UPDATE FAILED" with the reason. `otaScreenActive()` keeps the status line and menu from drawing over it.
- **Wi-Fi credentials** live in NVS (`kbd`/`wifiSsid`, `wifiPass`). To set them, hold the knob at power-on. The OLED shows the hotspot password; open `http://192.168.4.1` and fill in the Wi-Fi form. The same page has a manual `.bin` upload for recovery.
- **Dev builds:** local builds have `FW_VERSION=0` (`scripts/fw_version.py`), which turns auto-update off, so USB-flashed test firmware is not overwritten.
- **TLS roots:** `GithubRootCAs.h` pins the Sectigo/USERTrust (github.com), ISRG/Let's Encrypt (release-assets host) and DigiCert roots. If GitHub changes CA, update this file.
- **E-Ink / knob pin sharing:** the E-Ink SCK and the encoder CLK are both wired to GPIO18. `einkWake()` suspends the encoder interrupts and attaches SPI; `einkSleep()` hibernates the panel, calls `SPI.end()` and resumes the encoder. The knob doesn't respond during a refresh (~2-3 s), so idle redraws wait 3 s after the last key/knob input, and Clock mode is disabled. Moving the E-Ink SCK wire to a free GPIO and setting `EINK_SCK` (`HardwareConfig.h`) removes the sharing.
- **E-Ink "Animation" idle mode (`EinkAnimation.{h,cpp}`):** a storm scene (cloud, rain, cat under an umbrella) that starts after 5 s idle and loops through acts showing each refresh type: 1 FULL (flashing, ~2 s), 2 PARTIAL whole screen, 3 partial WINDOW (bottom strip only), 4 FLASH (full refresh of an inverted frame = lightning), 5 CLEAN (full refresh clears ghosting). The bottom HUD shows the act, frame number and last refresh time; Serial logs the average per act. The panel is not hibernated between frames. `einkAcquireBus()`/`einkReleaseBus()` move only SPI, so the knob works in the 150 ms gap between frames. Any key/knob input stops it, and it stops on a clean full refresh after 3 min.
- **No automatic rollback:** a release that crashes at boot needs a USB reflash. Flash usage is ~92% of the 1.9 MB OTA slot.
