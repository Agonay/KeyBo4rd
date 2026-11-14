# K3YBo4rd firmware

ESP32 Dev Module, 4 MB flash, Spanish (Spain) host layout. See AGENTS.md for matrix wiring.

## Build and install

Install PlatformIO, then run `pio run`. Connect USB and run `pio run -t upload` for the **first installation of this version**. This installs the OTA-capable partition table as well as the firmware. Do not install this first version through an older updater with a different partition table. Settings in the `kbd` NVS namespace are retained by ordinary uploads (do not erase flash).

Build output: `.pio/build/esp32dev/firmware.bin`. The two application slots each hold 2,031,616 bytes (`partitions.csv`: `min_spiffs` without SPIFFS). PlatformIO checks that the image fits. OTA never rewrites the partition table: a keyboard only gets the larger slots after one USB upload, and keyboards still on the old table (1,966,080-byte slots) reject images larger than that. The ESP32 platform is pinned to the verified version; library version ranges remain compatible with the existing dependency cache.

## Subsequent browser updates

1. Build with `pio run`.
2. Hold the knob button while powering on/resetting the keyboard for at least 1.5 seconds.
3. Read the temporary Wi-Fi password on the OLED (also printed at 115200 baud). Join `K3YBo4rd-Update` from the computer. Remain connected if the computer warns that this network has no Internet.
4. Open `http://192.168.4.1`, select **firmware.bin**, and install. Do not upload bootloader.bin, partitions.bin, or a merged flash image.
5. Keep power connected until the keyboard restarts. Rejoin the usual Wi-Fi network afterward.

Update mode expires after five minutes. Reset normally to cancel it. BLE is not started in update mode; Wi-Fi is not started during normal typing. The network password changes every boot into update mode. Access to this temporary WPA2 network grants permission to upload firmware; only join from a trusted device. Images are validated by ESP32 Update but are not cryptographically signed. OTA writes the inactive application slot; it does not implement automatic recovery from an application that boots but is faulty. USB upload remains the recovery path.

## Spanish keys

Configure the host for **Spanish (Spain)**, not Latin American or US International. The firmware sends HID usages directly and the host supplies characters, dead keys, Caps Lock, Shift and AltGr. Left Alt remains distinct from AltGr.

The custom matrix positions are preserved, including dedicated `[` and `]` positions after P. These send AltGr plus the Spanish grave/plus key positions. The bottom dedicated backslash sends AltGr plus the ordinal position. `<`/`>` uses HID non-US backslash (0x64), so the missing ISO key is available on this custom ANSI matrix. The acute/diaeresis position sends usage 0x34; apostrophe/question sends 0x2D; inverted exclamation/question sends 0x2E. Both Shift switches are combined, so releasing one does not release the other. Dedicated bracket keys use host Shift+AltGr behavior when shifted; they are not a US symbol layer that promises braces. Use normal Spanish AltGr combinations for braces.

Keys repeat according to the host's keyboard repeat settings. There is no firmware repeat or two-second hold limit. The 10 ms debounce and 4 ms scan target replace the old timing to allow normal typing. Scan time depends on I2C and display transfers. Six distinct non-modifier keys plus modifiers are supported by the existing HID descriptor; excess keys produce standard ErrorRollOver until enough are released.

## E-Ink / encoder wiring conflict

**E-Ink defaults to disabled.** ESP32 default SPI SCK is GPIO18, already connected to encoder CLK. SPI's default MISO19 also overlaps encoder DT; the display is write-only and is now configured without MISO. Using the shared clock can generate encoder interrupts and make input unstable.

Confirm the actual E-Ink SCK and MOSI wires before enabling it in `src/HardwareConfig.h`. Wire SCK to a genuinely unused output GPIO, set `EINK_SCK` accordingly, leave `EINK_MOSI=23` only if that matches wiring, then set `ENABLE_EINK=1`. The supplied pinout does not identify a verified spare GPIO, so no replacement is assumed. A compile-time guard rejects SPI assignments that overlap GPIO18/19. Check every other matrix, display, I2C, button, and strapping pin when choosing a replacement.

The display's busy callback services matrix scanning while waiting for E-Ink refresh. Drawing/SPI transfer and OLED transfers still take time; this is not a hard real-time scanner. Boot display drawing happens before BLE starts. Menus stay on the OLED; no E-Ink menu redraw is called from the main loop.

## Bluetooth changes and diagnosis

The former 500 ms supervision timeout could turn brief radio fades into disconnects. The firmware requests 15-30 ms intervals, zero peripheral latency, and a 6 second timeout; the host decides the actual parameters. Advertising and connection TX power are requested at +9 dBm. This improves transmit link margin at the cost of higher peak current and does not improve the ESP32 receiver sensitivity or guarantee radio coverage. No Wi-Fi runs in typing mode.

One complete keyboard report is sent per changed matrix snapshot, avoiding US ASCII translation, modifier ordering errors, repeated press calls and unrelated release calls. Connection settling clears keyboard and media reports and suppresses keys held across reconnection until all keys are released. Disconnected typing is intentionally not replayed later. Volume changes use a bounded queue with 20 ms between media press/release reports rather than notification bursts.

At 115200 baud, capture `[BLE] connected`, `ready`, and `disconnected` messages during a failure. The firmware cannot detect a physically missed matrix press after it has happened or prove from `isConnected()` that the host received a notification. This library does not expose delivery confirmation through sendReport. No idle key notifications are sent as keepalives: BLE already manages link supervision.

### Automated verification

`python tests/windows_layout.py` on Windows checks the actual firmware key definitions against Windows Spanish (Spain), without injecting key events. Verified: 22 character cases and both acute/diaeresis dead-key cases. This verifies layout conversion, not the electrical matrix, BLE delivery, or dead-key composition in applications.

### Hardware acceptance checks (not performed automatically)

- Type `ñ Ñ º ª ' ? ¡ ¿ , ; . : - _`, acute+vowel, diaeresis+vowel, AltGr+2 (`@`), AltGr+E (euro), AltGr+3 (`#`), and the dedicated brackets/backslash/less-than keys in a plain text editor. Confirm native Spanish braces too.
- Press Shift and a letter together, hold either/both Shift switches, release only one, and test Ctrl+C/Ctrl+V and Left Alt shortcuts. Hold arrows/letters for more than ten seconds; release must stop repeat immediately.
- Disconnect/reconnect BLE while holding a letter and while holding a modifier. Release all keys, then type. Old keys must not replay. Test media volume while typing and rapid knob rotation.
- Compare typing with lid open/closed at the same distance, then with keyboard beside the laptop and farther away. Check whether serial reports disconnects or merely missed input. Repeat with E-Ink disabled. These comparisons help separate weak RF link margin from scanner/display faults.
- If only closed-lid reception remains poor, inspect the ESP32 antenna clearance from metal, battery, ground planes and cables; compare stable USB power with battery power. A weak antenna/power implementation can affect this keyboard while other Bluetooth devices work correctly. Do not interpret the firmware changes as proof that RF problems are solved.
- Exercise all idle display modes after resolving the SPI wiring. Type while clock updates occur.
- Perform a browser update, cancellation, and a rejected invalid-file upload; verify normal typing after reboot. Do not interrupt flash power as a casual test.

## Other review findings

Battery readings are simulated (one percent per minute, wrapping to 100). They must not be used to diagnose supply voltage or low battery. Real ADC/divider/calibration work is still required. Matrix scanning assumes suitable switch diodes; no software ghost suppression is implemented. GPIO12/15 and other ESP32 strapping pins require correct boot-time electrical levels. Menu values now persist when leaving edit mode or closing the menu, avoiding a flash write for every knob movement. The encoder ISR lookup table is placed in RAM for flash-cache-disabled periods, and the knob button is debounced. Some menu values still ignore rotation direction; that is a separate usability improvement rather than an established cause of BLE dropouts.

References: [BLE Keyboard source](https://github.com/T-vK/ESP32-BLE-Keyboard/blob/master/BleKeyboard.cpp), [Espressif GAP parameters](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html), [Espressif browser OTA workflow](https://docs.espressif.com/projects/arduino-esp32/en/latest/ota_web_update.html).
