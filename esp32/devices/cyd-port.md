# ESP32-2432S028R "Cheap Yellow Display" port

A personal port of the Muse Gadget firmware to the ESP32-2432S028R
("Cheap Yellow Display"), following `devices/AGENTS.md` as a status-screen
board: `devices/sdkconfig.cyd`, `partitions_cyd_4mb.csv` and the
`HOMEHUB_LED_BACKEND_CYD_ILI9341` backend in `main/led_status.c`.

## Pin map

| Signal | GPIO | Notes |
|---|---|---|
| TFT MOSI | 13 | SPI2_HOST |
| TFT MISO | 12 | not used (MISO=-1) |
| TFT CLK | 14 | SPI2_HOST |
| TFT CS | 15 | |
| TFT DC | 2 | |
| TFT RST | -1 | not connected on this board |
| Backlight | 21 | driven high at boot (hard-wired on some variants) |
| Touch (XPT2046) | shared SPI | not used in the status-screen path |
| BOOT button | 0 | active low, setup/pairing button |
| USB bridge | — | CH340 on most variants (CP210x on some): `usbserial`/`wchusbserial`/`ttyUSB*` |

## What works

- Status screen: edge bars, breathing/pulse colours, pixel-art animation and
  the agent name on the 320x240 (landscape) display.
- `display.draw_url` / `display.show_animation` images, same as the other
  status-screen backends.
- Full Link control session: Wi-Fi setup, pairing, commands, text replies.
- BLE advertises as `MuseGadget-Disp-XXXXXX`.

## What doesn't

- No PSRAM, so `CONFIG_HOMEHUB_TUNNEL=n`: no home-network tunnel and no
  network discovery, and voice notes go over Link's session with text-only
  replies (same tradeoff as the ideaspark).
- Touch (XPT2046) is not driven: the status screen has no buttons to press
  anyway; the physical BOOT button handles pairing.
- 4 MB flash: the standard 8 MB partition table does not fit, so the board
  uses `partitions_cyd_4mb.csv` (two OTA app slots of 0x1E0000 each).

## Pairing flow

1. `tools/board.sh cyd build` (leave the SDK token empty only to check the
   build warns; set `CONFIG_GADGET_SDK_TOKEN` to pair for real), then
   `tools/board.sh cyd flash /dev/ttyUSB0`.
2. The screen breathes orange and BLE advertises as `MuseGadget-Disp-XXXXXX`.
3. In the Muse app, add a community device (community pairing v5), pick it
   from the scan list, and confirm with a short press of the BOOT button when
   the screen breathes blue.
4. Green dot top-right once the control session is up. Hold BOOT for 5 s to
   reset setup.

Note: `flash` writes the partition table, bootloader and the app to `ota_0`;
NVS is left alone, so pairing and Wi-Fi credentials survive a reflash.
