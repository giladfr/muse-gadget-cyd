# CYD Dashboard (fork notes)

A fork of Meta's `muse-gadget-sdk` that turns the ESP32-2432S028R ("Cheap
Yellow Display") into a touch dashboard for Muse.

## What it is

- **3.2" status screen** → **touch dashboard**: stocks, weather, calendar.
- Swipe left/right or tap the `<` `>` zones to switch screens.
- Muse pushes calendar data (`dashboard.data`) and full-screen images
  (`dashboard.takeover` / `display.draw_url`); an **X** button dismisses the
  takeover and returns to the dashboard.
- Stocks/weather come from a small bridge service on the home NAS
  (`bridge/`), polled over plain HTTP — the board has no PSRAM for TLS.
- OTA enabled; version 1.0.0. Future versions push from the cloud.

## Layout

```
esp32/                  ESP-IDF app (the SDK fork)
  main/dashboard/       dashboard UI: screens, touch, net, store
  main/led_status.c     display hooks (dashboard owns the panel when active)
  main/app.c            dashboard.* command handlers
  main/noise_control.cpp  command registration
  devices/sdkconfig.cyd board config (dashboard + OTA on)
bridge/                 NAS service: Nasdaq + Open-Meteo → plain-HTTP JSON
```

## Build

```sh
cd esp32
export IDF_PATH=~/esp/esp-idf-v6 && . $IDF_PATH/export.sh
tools/board.sh cyd build
```

## Flash (USB, from a Mac)

```sh
esptool.py --chip esp32 --port /dev/cu.usbserial-1230 write_flash \
    0x0 cyd-dashboard-v1.0.0.bin
```

The board reboots, shows the status screen until paired, then the dashboard
takes over on the first Link connection.

## Bridge (on the NAS)

```sh
cd bridge
docker compose up -d --build
curl http://<nas-ip>:8080/health   # {"ok": true}
```

Set `HOMEHUB_DASHBOARD_BRIDGE_URL` to `http://<nas-ip>:8080/dash.json`
(defaults to `http://192.168.1.100:8080/dash.json`).

## Commands (advertised to Muse)

- `dashboard.data` `{screen: "calendar", json: "{...}"}` — push calendar events.
- `dashboard.data` `{screen: "bridge", json: "{...}"}` — push stocks/weather.
- `dashboard.takeover` `{url}` — full-screen image with X dismiss.
- `display.draw_url` — also enters takeover mode in dashboard builds.
- `display.show_animation` — ends takeover, back to the dashboard.

## Debugging (black screen)

The firmware logs to UART0 at 115200 baud through the CH340. On a Mac:

```sh
screen /dev/cu.usbserial-1230 115200
```

(Detach with `Ctrl-A` then `Ctrl-\`.) Or: `idf.py -p /dev/cu.usbserial-1230 monitor`.

What to look for when the screen stays black:
- `dash.display: set_active...` — the dashboard took over the panel.
- `dash: dashboard init` / `dash: activating dashboard` — task lifecycle.
- `dash.touch: XPT2046 ready` — touch controller answering.
- `LED status ready: ... display (BL=21)` — panel + backlight init.
- A Guru Meditation / panic trace — boot loop; capture the backtrace.
- No output at all — check the USB cable (must be data, not charge-only)
  and that the board is getting enough power.

If the status screen shows but the dashboard never appears, the board has not
paired / connected yet (`dash: activating dashboard` never logged).

## Upstream

Forked from `facebookincubator/muse-gadget-sdk` at `b139b45`, plus the CYD
port and the dashboard work on the `dashboard` branch. Dashboard code is
isolated in `main/dashboard/`; SDK modifications are the display hooks in
`led_status.c`, the command handlers in `app.c`/`noise_control.cpp`, and the
Kconfig options.
