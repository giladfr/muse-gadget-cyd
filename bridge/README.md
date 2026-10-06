# dash-bridge

Data helper for the CYD dashboard (ESP32-2432S028R, 320x240).

**How it's used:** Muse's VM runs `fetch.py`, which fetches weather from
Open-Meteo (and stock quotes from Nasdaq) and prints one compact JSON
document; Muse pushes it to the board with `dashboard.data screen="bridge"`.
`fetch.py` imports `bridge.py`, so keep the two together.

Live stock quotes during market hours don't need this: the board fetches
them itself (see `DASHBOARD.md`, "Live stock quotes"), and ignores pushed
stocks while it does.

Optionally, `bridge.py` also runs as a small always-on server the board can
poll over plain HTTP (`CONFIG_HOMEHUB_DASHBOARD_BRIDGE_POLL`), e.g. with
`docker compose up -d --build` on a home server. Nothing requires it.

## Endpoints

### `GET /dash.json`

```json
{
  "stocks": [
    {"symbol": "AMD", "price": 631.75, "change": -2.16, "changePct": -0.34,
     "market": "Closed"}
  ],
  "weather": {
    "location": "Austin, TX", "temp": 78, "feels": 80, "desc": "Partly cloudy", "code": 2,
    "humidity": 55, "wind": 8,
    "forecast": [
      {"day": "Today", "high": 85, "low": 66, "code": 2},
      {"day": "Tue",   "high": 87, "low": 68, "code": 0}
    ]
  },
  "stocks_updated": 1728000000,
  "weather_updated": 1727999400,
  "updated": 1728000000,
  "now": 1728000012
}
```

- `stocks` refreshes every 30 s from Nasdaq's quote API
  (`api.nasdaq.com/api/quote/{sym}/info`), and every 10 min while every
  quote says the market is closed. Each symbol remembers which asset class
  (stocks / etf / index) answered, so ETFs (SPY, QQQ) cost one request.
- `weather` refreshes every 15 min from Open-Meteo (Austin TX,
  imperial units). `code` is the Open-Meteo weather code; `desc` is a
  short human label.
- `stocks_updated` / `weather_updated` are unix timestamps of each feed's
  last successful refresh (`0` = no data yet); `updated` is the newer of the
  two, for older clients. `now` is the bridge's clock when the document was
  built, so the board (which has no clock) shows the quotes' age as
  `now - stocks_updated` plus the time since it received them.
- A failed upstream fetch never wipes the cache — the last good data keeps
  being served.
- Only fields the board draws are sent: the document goes through the
  cloud as an escaped string and is parsed on a board without PSRAM.

### `GET /health`

```json
{"ok": true}
```

## How the ESP32 uses it

Normally a cloud cron runs `fetch.py` and pushes its output with
`dashboard.data screen="bridge"`. With `CONFIG_HOMEHUB_DASHBOARD_BRIDGE_POLL`
the board instead GETs `http://<nas-ip>:8080/dash.json` every 60 seconds
(plain HTTP, no TLS; a 4 KB buffer). The stocks header shows `live` for
quotes under 90 s old, then `Nm ago` / `Nh ago`.

## Config

| Env var   | Default                        | Meaning                        |
|-----------|--------------------------------|--------------------------------|
| `WATCHLIST` | `AMD,NVDA,AAPL,MSFT,SPY,QQQ` | Comma-separated stock symbols  |
| `PORT`      | `8080`                       | Listen port                    |
| `LAT`/`LON` | `30.2672` / `-97.7431`       | Weather coordinates (Austin)   |
| `TZ`        | `America/Chicago`            | Weather timezone               |
| `LOCATION`  | `Austin, TX`                 | Place name on the weather screen |

Stdlib only — no pip dependencies. Logs are quiet by design.
