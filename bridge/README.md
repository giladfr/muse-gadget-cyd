# dash-bridge

Small bridge service for the CYD dashboard (ESP32-2432S028R, 320x240).

**Why it exists:** the dashboard board has no PSRAM, so it cannot do TLS.
It can't call Nasdaq or Open-Meteo directly. This service runs on the home
NAS (always on, same LAN), polls those HTTPS APIs, and re-serves one
compact JSON document over **plain HTTP** that the board polls.

## Run on the NAS

On the Beelink (OpenMediaVault with Docker Compose):

```sh
cd /path/to/bridge
docker compose up -d --build
```

Check it's alive: `curl http://<nas-ip>:8080/health` → `{"ok": true}`

Change the watchlist without rebuilding:

```sh
WATCHLIST=AMD,NVDA,TSLA docker compose up -d
```

## Endpoints

### `GET /dash.json`

```json
{
  "stocks": [
    {"symbol": "AMD", "name": "Advanced Micro Devices",
     "price": 631.75, "change": -2.16, "changePct": -0.34,
     "market": "Closed"}
  ],
  "weather": {
    "temp": 78, "feels": 80, "desc": "Partly cloudy", "code": 2,
    "humidity": 55, "wind": 8,
    "forecast": [
      {"day": "Today", "high": 85, "low": 66, "code": 2, "desc": "Partly cloudy"},
      {"day": "Tue",   "high": 87, "low": 68, "code": 0, "desc": "Clear"}
    ]
  },
  "updated": 1728000000
}
```

- `stocks` refreshes every 30 s from Nasdaq's quote API
  (`api.nasdaq.com/api/quote/{sym}/info`), trying asset classes
  stocks → etf → index, so ETFs (SPY, QQQ) and indexes work too.
- `weather` refreshes every 15 min from Open-Meteo (Austin TX,
  imperial units). `code` is the Open-Meteo weather code; `desc` is a
  short human label.
- `updated` is a unix timestamp of the last successful refresh of
  either feed (`0` = no data yet). A failed upstream fetch never wipes
  the cache — the last good data keeps being served, so the board can
  treat a stale `updated` as "showing old data".

### `GET /health`

```json
{"ok": true}
```

## How the ESP32 uses it

The firmware polls `http://<nas-ip>:8080/dash.json` every 30–60 seconds
with a plain HTTP GET (no TLS), parses the JSON with cJSON, and renders
the stocks and weather screens. If `updated` is older than ~10 minutes
(or 0), it shows a small "stale" indicator instead of blanking the data.

## Config

| Env var   | Default                        | Meaning                        |
|-----------|--------------------------------|--------------------------------|
| `WATCHLIST` | `AMD,NVDA,AAPL,MSFT,SPY,QQQ` | Comma-separated stock symbols  |
| `PORT`      | `8080`                       | Listen port                    |
| `LAT`/`LON` | `30.2672` / `-97.7431`       | Weather coordinates (Austin)   |
| `TZ`        | `America/Chicago`            | Weather timezone               |

Stdlib only — no pip dependencies. Logs are quiet by design.
