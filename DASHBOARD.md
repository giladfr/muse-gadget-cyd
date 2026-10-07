# CYD Dashboard (fork notes)

A fork of Meta's `muse-gadget-sdk` that turns the ESP32-2432S028R ("Cheap
Yellow Display") into a touch dashboard for Muse. Built to live sealed in a
case: firmware and SD-card assets update over the air, and a bad update rolls
itself back.

![Dashboard screens](docs/dashboard/screens.png)

![Price flash and slide transitions](docs/dashboard/demo.gif)

## What it is

- **Screens:** stocks (with sparklines and a flash when a price moves),
  weather (icons, 4-day forecast), today's and tomorrow's calendar (past
  events dimmed, the next one highlighted), and an analog clock with Israel
  time under it. Long lists switch to compact rows.
- **Header:** clock (SNTP + your time zone), Link status dot, Wi-Fi bars.
- **Touch:** swipe or tap the `<` `>` zones; screens slide. Hold a finger
  down for 5 s to calibrate touch.
- **Backlight:** follows the room's light sensor, dims at night, wakes on
  touch.
- **Takeover:** Muse can show a full-screen image (`dashboard.takeover`,
  `display.draw_url`, `display.draw_sd`); **X** closes it, and it returns to
  the dashboard by itself after 10 minutes.
- **Data:** live stock quotes fetched by the board itself during market
  hours (Nasdaq, no API key); weather and the calendar pushed by Muse with
  `dashboard.data`. Nothing runs anywhere except the board and Muse's VM.
- **Muse's own screens:** up to 4 cards (text, rows with progress bars and
  sparklines, buttons) and notification banners. A button tap goes back to
  Muse as a chat message, so Muse can ask and you answer with one tap.
- **Over the air:** firmware (`device.ota`) and SD-card files (`sd.fetch`).

Version: `esp32/version.txt` (1.5.5).

## Preview without a board

```sh
cd esp32
tools/dash_preview/run.sh        # needs cc, python3 with Pillow + numpy
open build-preview/screens.png build-preview/demo.gif
```

This compiles the real dashboard sources against stub ESP-IDF headers, plays
a scripted session (data pushes, a swipe, taps, calibration, an update, an
image), and renders every screen plus the animations. Run it after any UI
change; `docs/dashboard/` holds the committed copies shown above.

## Simulator: run the dashboard on your computer

`tools/dash_sim` runs the real dashboard firmware (`main/dashboard/`) in a
window. Only the chip layer is simulated: FreeRTOS tasks are threads, the
mouse drives a simulated XPT2046 on the touch pins (so the real touch
driver, gestures and calibration run), HTTP goes through libcurl (live
Nasdaq quotes), NVS is a file, and chat sends to Muse are printed.

```sh
brew install sdl2 cjson            # macOS (Ubuntu: libsdl2-dev libcurl4-openssl-dev libcjson-dev)
cd esp32
tools/dash_sim/build.sh
build-sim/dash_sim                 # click = touch, drag = swipe
```

Type the same commands Muse sends into its console, e.g.
`dashboard.card {"json": {"id": "t", "title": "Test", "buttons": [{"id": "ok", "label": "OK"}], "show": true}}`,
or `help` for the test commands (`tap`, `swipe`, `ldr` for the light sensor,
`link off`, `chat fail`, `ota 40`, `image file.bmp`, `snap file.bmp`).
Keys: arrows swipe, C calibrate, B/D bright/dark room, L link, S screenshot.
`--swap-touch --invert-x --invert-y` emulate a panel mounted another way;
`--slow-spi` paces drawing like the real 40 MHz bus.

`tools/dash_sim/test.sh` runs the scripted tests in `tools/dash_sim/tests/`
headless (cards and taps, offline behaviour, calibration, the Muse bridge
below); CI runs them, the preview and the quote test on every pull request.

What it doesn't cover: the ESP32 itself (RAM limits, timing, Wi-Fi, TLS),
the panel and SD card drivers, OTA, and the firmware's own Link session code.
Those still need the board (or a build in CI).

### Driving the simulator from your real Muse (Mac)

Your Mac can pair with Muse as a gadget of its own, "CYD Dashboard
(simulator)". Muse then sees the CYD's `dashboard.*` commands on it, and
whatever it sends lands in the simulator window. Taps on card buttons go back
to Muse as chat messages, as they do from the board. Use it to try the Muse
side, such as the skill, proactive cards or button answers, before the board
is on the desk.

```sh
cd esp32
tools/dash_sim/mac_gadget.sh pair --sdk-token mgst_...   # once; then add a device in the Muse app
tools/dash_sim/mac_gadget.sh                             # simulator + Link session
```

How it works: pairing uses the repo's Linux Device SDK (`linux/`) with a
CoreBluetooth backend (`linux/src/musegadget/ble_server_macos.py`), so the
Mac's own Bluetooth does the setup. After that `musegadget dash-sim` holds
the Link session over the internet and forwards Muse's commands to
`dash_sim --listen 8765`. `dash_sim --muse-socket` hands button taps to it.
The script keeps its state, venv and pairing in `~/.musegadget-sim`, and
`mac_gadget.sh unpair` forgets the pairing. The gadget offers only the
dashboard commands, never shell or file access to the Mac.

Mac specifics:
- Allow the terminal under System Settings > Privacy & Security > Bluetooth.
- macOS apps can't advertise manufacturer data, so the advert has the name
  and setup service but not the "not paired yet" flag the board sends.
- The phone checks the Bluetooth device name, which on a Mac is the computer
  name. So `pair` renames the Mac to the gadget's name (`MuseGadgetXXXXXX`)
  for the setup window and renames it back afterwards (sudo). If the app
  still shows the old name, toggle Bluetooth once. `--keep-name` skips the
  rename.

Untested on a real Mac and phone so far. If the Muse app doesn't list the
gadget, the likely cause is the missing advert flag. Pairing from a Linux
machine or Raspberry Pi with `musegadget pair` (BlueZ) works around it.
Run `tools/dash_sim/mac_gadget.sh` there too; only the rename is Mac-only.

## Hardware (ESP32-2432S028R)

| Part | Pins | Driven by |
|---|---|---|
| ILI9341 display | SCK 14, MOSI 13, CS 15, DC 2 | SPI2_HOST (`led_status.c`) |
| Backlight | 21 | LEDC PWM (`dash_backlight.c`) |
| Light sensor (LDR) | 34 | ADC1 channel 6 |
| XPT2046 touch | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 | bit-banged (`dash_touch.c`) |
| SD card | SCK 18, MOSI 23, MISO 19, CS 5 | SPI3_HOST (`sd_card.c`) |

Touch and SD do **not** share the display's bus.

## Hardware bring-up (v1.4.1–v1.4.5, Oct 2026)

The ESP32-D0WD has 4 MB flash and **no PSRAM**. The dashboard + Link stack
push internal RAM to the limit. These fixes were needed to get the first
board online:

- **v1.4.1 — Lean Link session.** The session needed ~80 KB of buffers; the
  largest free block was ~64 KB (`session buffer alloc failed`). A
  dashboard-specific lean mode cuts it to ~36 KB (4 KB WS TX, 8 KB WS RX,
  4 KB + 4 KB scratch, 2× 8 KB inbound, 2 KB chunks).
- **v1.4.2 — Double-free in `build_register_json`.** A `take_required` cJSON
  object was added to 5 commands; cJSON takes ownership on add, so the tree
  free freed it 5× (`tlsf_free` assert → reboot loop). Each command now gets
  its own param objects. Also fixed SD commands to advertise `path`, not `url`.
- **v1.4.3 — Register JSON 8 KB → 16 KB.** The dashboard's command
  descriptions pushed the JSON past the 8 KB print buffer
  (`could not build link.register`).
- **v1.4.4 — Deferred dashboard RAM.** During BLE pairing, BLE + WiFi + TLS
  exhausted the DMA heap (`wifi:mem fail`, 0K free). Strip buffers (5 KB) and
  the render task (5 KB stack) now allocate on `dashboard_set_paired(true)`
  instead of at boot.
- **v1.4.5 — Night backlight 5% → 25%.** At 5% the quadratic brightness curve
  gives a PWM duty of ~2/1023 — essentially black.

## Boot splash (v1.5.5)

![Boot splash](docs/dashboard/splash.gif)

Muse's mascot pops up from a squash, opens its eyes and cheers with hearts.
"Muse Dashboard" rises in under it, then the version, and three dots pulse
while the Link comes up. It runs 3.4 s; taps during it are ignored.

- **Frames:** pre-rendered at build time from the firmware's own avatar
  renderer (`avatar/muse_pixel.c`) by
  `main/dashboard/assets/gen_splash.py`. Running that renderer live would
  cost ~12 KB of RAM permanently. There are 35 frames at 12 fps, 64×64 in
  15 colours plus transparent, each stored as a delta from the previous one.
  That's 18.9 KB of flash (`dash_splash_anim.c`), drawn at 3×.
- **RAM:** one 2 KB 4-bit frame buffer, allocated when the splash starts and
  freed when it ends. Without it, the old static title and version splash is
  shown instead. Decoding goes straight into the existing strip buffers.
  `dash_splash.c` is ~1 KB of code and 12 bytes of static RAM.
- **Regenerate:** `python3 main/dashboard/assets/gen_splash.py` (from
  `esp32/`, needs a C compiler and numpy). It decodes every frame back to
  check it. Like the avatar, the artwork isn't covered by the Apache
  License.
- **Watch it:** in the simulator (`build-sim/dash_sim`), or record it with
  `record 4400 frame` in its console (a BMP every 40 ms).

## Clock screen (v1.5.0–v1.5.4)

- **v1.5.0/v1.5.1 bug — "Nothing scheduled" on the Clock screen, green
  artifacts.** `draw_clock` called `dash_line(..., DASH_WHITE, 3)`, but the
  signature is `dash_line(..., float width, uint16_t colour)`. Each tick and
  hand became a line 65535 px wide in colour `0x0003` (green on this panel,
  which takes the high byte first). Every strip then ran its distance test
  over ~65,000 columns per row: 867 ms per frame on a desktop PC, minutes on
  the ESP32 (no hardware square root). The slide into the Clock screen draws
  the header and dots first, then crawls through the content, so the
  previous screen's "Nothing scheduled" stayed up. It was never memory
  corruption, and `s_animating` only made it redraw that slow frame forever.
- **v1.5.2/v1.5.3 Israel time leaked RAM.** `dash_clock_israel()` switched
  `TZ` with `setenv()`. newlib allocates a new string when the value grows
  (Israel's TZ is longer than Chicago's) and never frees the old one, so each
  redraw leaked ~30 bytes of internal RAM. Other tasks could also read Israel
  time while `TZ` was switched. v1.5.4 computes it from UTC (checked against
  zoneinfo on 1.1 M timestamps, 2024–2040).
- **v1.5.4 analog clock:** face, 60 ticks, hour/minute hands and an amber
  second hand. The hand tips are computed once per frame. Each second only
  the rows the hands sweep are repainted (about 110 of 240), and the Israel
  line only when its minute changes. On the Clock screen the dashboard wakes
  on each wall-clock second instead of the 5 s header tick. The v1.5.2
  digital clock drew its time in the 58 px font, whose glyphs are only
  digits, space and minus, so colons and AM/PM were missing.
- The boot splash now ends on time (it waited for the next 5 s tick) and
  repaints the screen when it does. Taps during the splash are ignored.

**Pairing notes:**
- Pairing needs BLE + WiFi + TLS simultaneously. If it fails with
  `wifi:mem fail`, the board is out of DMA RAM — the v1.4.4+ deferred
  allocation should prevent this.
- To wipe WiFi/pairing without erasing firmware:
  `esptool.py --chip esp32 --port /dev/cu.usbserial-11230 erase_region 0x9000 0x6000`
  (NVS partition). Or hold BOOT (GPIO 0) for 5 s.
- App-only flash (`write_flash 0x20000`) preserves NVS, WiFi, and pairing.

**OTA notes:**
- `device.ota` uses `esp_https_ota`. HTTPS works for small API calls, but the
  TLS session for a 1.8 MB download does **not** fit in RAM
  (`ESP_ERR_HTTP_CONNECT`). Serve the `.bin` over plain **HTTP** — the image
  is RSA-signed, so HTTP is safe.
- Example: `python3 -m http.server 8000` on a Mac on the same LAN, then
  `device.ota {"url": "http://192.168.50.x:8000/cyd-dashboard-vX.Y.Z.bin"}`.

## Settings (Kconfig, "ESP32 Device SDK")

Put personal values in `esp32/devices/sdkconfig.local` (git-ignored, see
`sdkconfig.local.example`); `tools/board.sh` loads it last.

| Option | Default | |
|---|---|---|
| `CONFIG_GADGET_SDK_TOKEN` | — | **required** for OTA images |
| `HOMEHUB_DASHBOARD_TZ` | `CST6CDT,M3.2.0,M11.1.0` | POSIX time zone |
| `HOMEHUB_DASHBOARD_CLOCK_24H` | n | |
| `HOMEHUB_DASHBOARD_BL_AUTO` | y | follow the light sensor |
| `HOMEHUB_DASHBOARD_BL_LDR_BRIGHT` / `_DARK` | 50 / 600 | raw sensor readings; tune with `dashboard.debug` |
| `HOMEHUB_DASHBOARD_BL_NIGHT_START` / `_END` / `_PCT` | 23 / 7 / 25% | night dimming (same hour = off) |
| `HOMEHUB_DASHBOARD_BL_IDLE_MIN` / `_PCT` | 0 (off) / 30% | dim when untouched |
| `HOMEHUB_DASHBOARD_TAKEOVER_TIMEOUT_MIN` | 10 | 0 = images stay until X |
| `HOMEHUB_DASHBOARD_TOUCH_SWAP_XY` / `_INVERT_X` / `_INVERT_Y` | n | defaults until calibrated |
| `HOMEHUB_DASHBOARD_QUOTES` | y (CYD) | live quotes fetched by the board |
| `HOMEHUB_DASHBOARD_QUOTES_SYMBOLS` | `AMD,NVDA,AAPL,MSFT,SPY,QQQ` | up to 8 (or `dashboard.stocks`) |
| `HOMEHUB_DASHBOARD_QUOTES_OPEN_S` | 20 | refresh while the market is open |
| `HOMEHUB_DASHBOARD_BRIDGE_POLL` / `_URL` | n | poll a self-hosted `bridge.py` (not needed) |

## Updating over the air

No USB needed once 1.1.0 or later is on the board. **Flash the latest over
USB once before closing the case:** 1.0.5 only accepts `https://` OTA URLs (which may
not fit in its RAM) and has none of the safety checks below. If the case is
already closed, `device.ota` with an `https://` URL is worth a try; if it
fails, nothing is installed.

1. Bump `esp32/version.txt` (the board skips images that are not newer).
2. Build: `cd esp32 && tools/cyd_release.sh` → `release/cyd-dashboard-v<ver>.bin`.
   It refuses to build without the SDK token, and prints the size and the
   exact command.
3. Serve the `.bin` anywhere the board can reach, plain `http://` is fine,
   and run `device.ota {"url": "http://<host>/cyd-dashboard-v<ver>.bin"}`.

What happens on the board, and why it can't brick itself:

- The screen shows **Updating** with a progress bar.
- The image's RSA signature is checked before it is installed (the dev key in
  `esp32/dev_signing_key.pem`; the VM must build with the same key as the
  running firmware). That is why plain HTTP is safe, and HTTPS isn't needed
  (a second TLS session doesn't fit in this board's RAM).
- The new firmware boots on probation. It is accepted only once the Link
  session is back **and** the dashboard has drawn and run for 30 s. If it
  crashes first, or doesn't reconnect within 5 minutes, the bootloader goes
  back to the previous firmware.
- Even after acceptance, three crash resets in a row switch back to the other
  firmware slot (`ota_crash_guard_boot()`).

Asking Muse to do it end to end ("update the dashboard firmware") works if its
VM has ESP-IDF v6.0.1, this repo, `devices/sdkconfig.local` with the token,
and somewhere to serve the file over HTTP. The cyd board is also built in CI
(`.github/workflows/esp32.yml`) on pull requests.

## SD card over the air

`sd.fetch {"url", "path", "sha256"?}` downloads any file (up to 8 MB) to
`/sdcard/<path>`, writing to `<path>.part` and renaming only when complete,
and only if the SHA-256 matches when one is given. Then e.g.
`display.draw_sd {"path": "images/cat.jpg"}` shows it (baseline JPEG).
`sd.list`, `sd.read`, `sd.write`, `sd.remove` and `sd.info` cover the rest.

## Commands (advertised to Muse)

- `dashboard.data` `{screen: "bridge" | "calendar", json}`: push data. `json`
  may be a string or an object (also accepted as `data`). An empty `stocks`
  list or a missing `weather` keeps the last good data.
  Calendar: `{"label": "Tuesday, Oct 6", "events": [{"time": "5:45 PM", "title": "..."}]}`.
- `dashboard.takeover` `{url}`, `display.draw_url`, `display.draw_sd {path}`:
  full-screen image with X. `display.show_animation` closes it.
- `dashboard.calibrate` `{reset?}`: touch calibration screen (or forget it).
- `dashboard.debug`: touch raw/pressure, light sensor, backlight, state, free
  heap.
- `dashboard.card`, `dashboard.notify`, `dashboard.events {peek?}`: see
  "Cards, banners and taps".
- `dashboard.stocks` `{symbols?}`: live-quote watchlist and status.
- `device.ota` `{url, force?}`, `sd.fetch`: see above.

## Live stock quotes

The board fetches its watchlist itself from Nasdaq's public quote API over
HTTPS, with **no API key**: the same source and request as DeskPulse
(browser-like headers; stocks → ETF → index asset classes, remembered per
symbol). Every 20 s while the US market is open (9:30-16:00 New York time),
every minute pre-/after-market (showing the extended-hours trade, like
DeskPulse), every 30 minutes when closed. Nasdaq's own market status covers
holidays.

- Each price move flashes its row; the sparkline is today's intraday chart
  (refreshed every 5 min, streamed and downsampled on the fly since the full
  chart is ~40 KB), with its tip following the live price.
- Change the watchlist with `dashboard.stocks {"symbols": "AMD,NVDA,SPY"}`
  (up to 8; saved on the board). With no params it reports status, as does
  `dashboard.debug`.
- While its own quotes arrive, the board ignores `stocks` in pushed
  `dashboard.data`, so Muse's VM only needs to push weather and the calendar.
- A symbol Nasdaq doesn't know is retried every 10 minutes; if Nasdaq refuses
  (HTTP 403/429) the board backs off for 5 minutes.

Memory: each round runs on a short-lived task with one keep-alive TLS
session (both freed afterwards). It is skipped while free RAM is under
~64 KB, and while an image or firmware update downloads. If `dashboard.debug`
shows `heap_skips` climbing, the board doesn't have the room; say so and we
can trim elsewhere.

`tools/dash_preview/run.sh` also runs `quotes_test.c`: a full round against
DeskPulse's Nasdaq fixtures, no network.

## Cards, banners and taps

Muse can add its own screens without a firmware update. The skill in
`skills/gadget-cyd-desk-dashboard/SKILL.md` teaches Muse how (point Muse at
it); the short version:

- `dashboard.card {"json": {id, title, sub?, text?, tone?, rows?, buttons?,
  ttl_s?, show?}}`: up to 4 cards after the built-in screens. Rows (up to 5)
  have `label`, `value`, `detail`, `tone` (up/down/accent/blue/dim),
  `progress` 0-100 and `spark` (numbers). Buttons (up to 3) have `id`,
  `label` and `say`. Same `id` replaces; `"remove": true` deletes.
- `dashboard.notify {text, detail?, level?, card?, ttl_s?}`: a one-line
  banner in place of the header for 20 s; a tap dismisses it or opens `card`.
- A button tap shows the button pressed and sends `[Desk display] <say>` to
  Muse as a text turn over the board's existing Link session
  (`POST /chat/stream` on the VM, the way Muse's own boards send text; no
  extra connection). The board only sends; Muse answers in the app or by
  updating the card ("Sent to Muse" appears under its title). Every tap is
  also queued for `dashboard.events`, in case the chat path is unavailable.
  `CONFIG_HOMEHUB_DASHBOARD_MUSE_CHAT` (default y) turns the chat send on,
  which also compiles the session's request streams for this build.

Untested on hardware: whether this VM accepts a text turn from the CYD the
way it does from Muse's own boards. `dashboard.debug` shows `events ...
sent= acked= failed= http=`; if `failed` grows, use `dashboard.events`.

## Weather from the Muse VM

Muse's VM runs `bridge/fetch.py` (it imports `bridge.py`, keep them together)
and pushes the output with `dashboard.data screen="bridge"`. Set `LOCATION`,
`LAT`, `LON`, `TZ` and `WATCHLIST` in its environment. `bridge.py` can also
run as a small server that the board polls (`HOMEHUB_DASHBOARD_BRIDGE_POLL`),
but nothing requires it.

## Build and first flash (USB, once)

```sh
cd esp32
export IDF_PATH=~/esp/esp-idf-v6 && . $IDF_PATH/export.sh
tools/board.sh cyd build
tools/board.sh cyd flash /dev/cu.usbserial-1230
```

## How it renders

- Only the dashboard task draws; other tasks change state and wake it.
- It sleeps between events (touch interrupt, data, a 5 s header tick), polls
  at 10 ms only while a finger is down, and at ~12 fps only while something
  animates.
- Frames are built once from a store snapshot and painted in 4-row strips,
  double-buffered over SPI DMA. Only rows that changed are repainted.
- Text is Inter (SIL OFL, `main/dashboard/assets/`), anti-aliased, stored
  as 4-bit alpha with the icons (~54 KB of flash). Regenerate with
  `assets/gen_assets.py`.
- Colours are RGB565 stored high byte first, the panel's byte order
  (`DASH_RGB` in `dash_draw.h`).

## Debugging

UART0 at 115200 through the CH340 (`screen /dev/cu.usbserial-1230 115200`),
or remotely with `dashboard.debug`. Useful log lines:
`dash: activating dashboard`, `dash: first frame drawn`,
`link: OTA image validated`, `link.ota: crash loop ...`, `dash.bl: ...`.

## Layout

```
esp32/main/dashboard/    dashboard: task, screens, drawing, touch, store,
                         clock, backlight, icons, generated assets
esp32/main/sd_*.c        SD card mount, JPEG viewer, sd.fetch downloads
esp32/main/ota.c         OTA (+ progress, crash-loop guard)
esp32/tools/dash_preview host preview (stub IDF headers)
esp32/tools/cyd_release.sh  build an OTA image
bridge/                  fetch.py: Nasdaq + Open-Meteo → JSON for Muse to push
```

## Upstream

Forked from `facebookincubator/muse-gadget-sdk` at `b139b45`. Dashboard code
is isolated in `main/dashboard/`; SDK changes are the display hooks in
`led_status.c`, the command handlers in `app.c`/`noise_control.cpp`, the
OTA additions in `ota.c`, and the Kconfig options.
