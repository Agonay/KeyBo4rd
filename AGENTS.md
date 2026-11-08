# KeyBo4rd — AGENTS.md

ESP32 custom mechanical keyboard with BLE HID, 6×13 matrix, rotary encoder knob, OLED + E-Ink displays.

## Hardware pinout

| Component | Pin / Address | Purpose |
|---|---|---|
| **Matrix rows** (ESP32 GPIO, INPUT_PULLUP) | 25, 33, 12, 26, 27, 14 | Row scan lines |
| **Matrix cols** (PCF8575 I2C expander, addr 0x20) | P15 P13 P4 P1 P3 P7 P5 P6 P11 P0 P2 P14 P12 | Column drivers |
| **Rotary encoder CLK** | 18 | Quadrature A |
| **Rotary encoder DT** | 19 | Quadrature B |
| **Rotary encoder SW** | 13 | Push button (active-LOW, PULLUP) |
| **E-Ink (GxEPD2 1.54" D67)** | CS=5 BUSY=15 RES=17 DC=16 | Static image / menu / dashboard |
| **OLED (SSD1306 128×64)** | I2C 0x3C | Caps lock, battery, status line |
| **I2C bus** | SDA=21 SCL=22 (default Wire) | OLED + PCF8575 |

## Matrix layout

```
Rows: 6 (ESP32 pins) × Cols: 13 (PCF8575 pins) = 78 physical positions
```

Row 0 = function keys (F1-F12 + Escape). Row 1 = number row. Rows 2-5 = QWERTY area.

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
Outside the menu:  turn = volume (spin fast = 2x/3x steps)
                   press = play/pause   double-press = mute
                   hold (0.8 s) = open menu   hold 2.5 s = releaseAll (stuck-key recovery)

Menu list:  turn = move   press = select / edit   hold = close
Editing:    turn right = +, turn left = -   press = save   hold = cancel (revert)
            The edited value shows as <value>. Auto-close after 20 s without input.

Items: Volume step (1-4), Direction, OLED bright (0-100%, live preview),
       Show battery, Low battery (5-50%), Idle screen (only if ENABLE_EINK),
       Check update (checks GitHub now, installs immediately if newer),
       About (firmware, Wi-Fi, IP, Bluetooth, uptime), < Exit
Header shows the firmware version (FW vN / FW dev).
```

- Encoder: full-cycle (4 transitions per click, rests at A=B=1). A click counts when the knob returns to rest, re-syncing every click. `-DENC_HALF_STEP` for half-cycle encoders; `-DENC_DEBUG` logs raw ticks.

## Firmware updates (OTA from GitHub)

- **Release flow:** a push to `master` that touches `src/**`, `platformio.ini` or `scripts/**` runs `.github/workflows/firmware.yml`. It builds with `FW_VERSION=<run number>` and publishes the release `fw-N` with `firmware.bin` and `version.json` (`{"version","md5","size","notes"}`; `notes` = commit title, shown on the update screen).
- **Device side (`OtaUpdater.{h,cpp}`):**
  - `otaBootCheck()` runs before `initBluetooth()`. It joins Wi-Fi, reads `releases/latest/download/version.json`, and if the release is newer, streams `firmware.bin` into the spare OTA slot (MD5-checked) and reboots.
  - `otaLoop()`: after 4 min idle (no keys/knob, menu closed), at most once an hour, a core-0 task checks the version while BLE stays up. If a newer one exists, the OLED shows a 10 s "Installing in Ns" countdown (any key/knob postpones to the next idle window), then it stores the version in NVS (`otaPendV`) and reboots, so the download happens in the boot path.
  - Update screen: title bar, `vA -> vB`, release notes, "Downloading x/y KB" + progress bar, "Verifying...", then "UPDATE COMPLETE" or "UPDATE FAILED" with the reason. `otaScreenActive()` keeps the status line and menu from drawing over it.
- **Wi-Fi credentials** live in NVS (`kbd`/`wifiSsid`, `wifiPass`). To set them, hold the knob at power-on. The OLED shows the hotspot password; open `http://192.168.4.1` and fill in the Wi-Fi form. The same page has a manual `.bin` upload for recovery.
- **Dev builds:** local builds have `FW_VERSION=0` (`scripts/fw_version.py`), which turns auto-update off, so USB-flashed test firmware is not overwritten.
- **TLS roots:** `GithubRootCAs.h` pins the Sectigo/USERTrust (github.com), ISRG/Let's Encrypt (release-assets host) and DigiCert roots. If GitHub changes CA, update this file.
- **No automatic rollback:** a release that crashes at boot needs a USB reflash. Flash usage is ~92% of the 1.9 MB OTA slot.
