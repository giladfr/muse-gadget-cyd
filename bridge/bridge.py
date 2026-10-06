#!/usr/bin/env python3
"""dash-bridge: plain-HTTP JSON bridge for the CYD dashboard.

The ESP32 dashboard board has no PSRAM and cannot do TLS, so it cannot
call HTTPS APIs itself. This service runs on the home NAS (always on,
same LAN), polls Nasdaq (stocks) and Open-Meteo (weather) over HTTPS,
and re-serves one compact JSON document over plain HTTP.

Endpoints:
    GET /dash.json  -> {"stocks": [...], "weather": {...}, "updated": <unix ts>}
    GET /health      -> {"ok": true}

Config via env:
    WATCHLIST   comma-separated symbols (default: AMD,NVDA,AAPL,MSFT,SPY,QQQ)
    PORT        listen port (default: 8080)
    LAT, LON    weather coordinates (default: 30.2672, -97.7431 = Austin TX)
    TZ          weather timezone (default: America/Chicago)

Resilience: a failed upstream fetch never crashes the service and never
wipes the cache -- the last good data keeps being served. Clients can
judge staleness from the "updated" unix timestamp (0 = no data yet).
"""

import concurrent.futures
import json
import os
import re
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WATCHLIST = [s.strip().upper() for s in
             os.environ.get("WATCHLIST", "AMD,NVDA,AAPL,MSFT,SPY,QQQ").split(",")
             if s.strip()]
PORT = int(os.environ.get("PORT", "8080"))
LAT = os.environ.get("LAT", "30.2672")
LON = os.environ.get("LON", "-97.7431")
TZ = os.environ.get("TZ", "America/Chicago")

STOCK_POLL_S = 30
WEATHER_POLL_S = 15 * 60
FETCH_TIMEOUT_S = 12

NASDAQ_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X) "
             "AppleWebKit/605.1.15 Safari/605.1.15")
ASSET_CLASSES = ("stocks", "etf", "index")

_state = {"stocks": [], "weather": None, "updated": 0}
_lock = threading.Lock()


# ---------------------------------------------------------------- fetching

def _http_get_json(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT_S) as resp:
        if resp.status != 200:
            raise RuntimeError("HTTP %d for %s" % (resp.status, url))
        return json.loads(resp.read().decode("utf-8"))


def _num(value):
    """' $631.75 ' / '-0.34%' -> float. None when unparseable."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    cleaned = re.sub(r"[^0-9.\-]", "", str(value))
    if cleaned in ("", ".", "-", "-."):
        return None
    try:
        return float(cleaned)
    except ValueError:
        return None


def _clean_company(name, symbol):
    cleaned = name or symbol
    for pat in (r"\s+(Class [A-Z]\s+)?(Common Stock|Ordinary Shares|Common Shares)$",
                r"\s+American Depositary Shares.*$",
                r",?\s+(Inc|Corp|Corporation|Ltd|Limited|Co|plc|N\.V|S\.A)\.?$"):
        cleaned = re.sub(pat, "", cleaned, flags=re.IGNORECASE)
    cleaned = cleaned.strip()
    return cleaned or symbol


def _fetch_quote(symbol):
    """One symbol via Nasdaq. Tries stocks -> etf -> index asset classes."""
    headers = {"User-Agent": NASDAQ_UA,
               "Accept": "application/json, text/plain, */*"}
    for asset in ASSET_CLASSES:
        try:
            url = ("https://api.nasdaq.com/api/quote/%s/info?assetclass=%s"
                   % (urllib.request.quote(symbol), asset))
            root = _http_get_json(url, headers)
            data = root.get("data") or {}
            primary = data.get("primaryData") or {}
            price = _num(primary.get("lastSalePrice"))
            if price is None:
                continue  # wrong asset class or bad payload; try next
            return {
                "symbol": symbol,
                "name": _clean_company(data.get("companyName"), symbol),
                "price": round(price, 2),
                "change": _num(primary.get("netChange")) or 0.0,
                "changePct": _num(primary.get("percentageChange")) or 0.0,
                "market": str(data.get("marketStatus") or "Unknown"),
            }
        except Exception:
            continue
    return None


def _wx_desc(code):
    try:
        code = int(code)
    except (TypeError, ValueError):
        return "Unknown"
    if code == 0:
        return "Clear"
    if 1 <= code <= 3:
        return "Partly cloudy"
    if code in (45, 48):
        return "Foggy"
    if 51 <= code <= 67:
        return "Rain"
    if 71 <= code <= 77:
        return "Snow"
    if 80 <= code <= 82:
        return "Showers"
    if code in (85, 86):
        return "Snow showers"
    if 95 <= code <= 99:
        return "Thunderstorms"
    return "Unknown"


def _fetch_weather():
    url = ("https://api.open-meteo.com/v1/forecast"
           "?latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
           "weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&temperature_unit=fahrenheit&wind_speed_unit=mph"
           "&timezone=%s&forecast_days=4"
           % (LAT, LON, urllib.request.quote(TZ, safe="")))
    root = _http_get_json(url)
    cur = root.get("current") or {}
    daily = root.get("daily") or {}
    times = daily.get("time") or []
    highs = daily.get("temperature_2m_max") or []
    lows = daily.get("temperature_2m_min") or []
    codes = daily.get("weather_code") or []
    forecast = []
    for i in range(min(4, len(times), len(highs), len(lows), len(codes))):
        try:
            dt = time.strptime(times[i], "%Y-%m-%d")
            label = "Today" if i == 0 else time.strftime("%a", dt)
        except ValueError:
            label = times[i]
        forecast.append({
            "day": label,
            "high": int(round(highs[i])),
            "low": int(round(lows[i])),
            "code": int(codes[i]),
            "desc": _wx_desc(codes[i]),
        })
    code = cur.get("weather_code")
    return {
        "temp": int(round(cur.get("temperature_2m", 0))),
        "feels": int(round(cur.get("apparent_temperature",
                                   cur.get("temperature_2m", 0)))),
        "desc": _wx_desc(code),
        "code": int(code) if code is not None else -1,
        "humidity": int(cur.get("relative_humidity_2m", 0)),
        "wind": int(round(cur.get("wind_speed_10m", 0))),
        "forecast": forecast,
    }


# ---------------------------------------------------------------- poll loops

def _poll_stocks():
    while True:
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
                quotes = [q for q in ex.map(_fetch_quote, WATCHLIST) if q]
            if quotes:  # keep last good data if every fetch failed
                with _lock:
                    # preserve watchlist order
                    order = {s: i for i, s in enumerate(WATCHLIST)}
                    quotes.sort(key=lambda q: order.get(q["symbol"], 99))
                    _state["stocks"] = quotes
                    _state["updated"] = int(time.time())
        except Exception:
            pass  # never die; try again next round
        time.sleep(STOCK_POLL_S)


def _poll_weather():
    while True:
        try:
            wx = _fetch_weather()
            with _lock:
                _state["weather"] = wx
                _state["updated"] = int(time.time())
        except Exception:
            pass
        time.sleep(WEATHER_POLL_S)


# ---------------------------------------------------------------- http server

class _Handler(BaseHTTPRequestHandler):
    server_version = "dash-bridge/1.0"

    def _send(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/dash.json":
            with _lock:
                self._send(dict(_state))
        elif path == "/health":
            self._send({"ok": True})
        else:
            self._send({"error": "not found"}, status=404)

    def log_message(self, *args):
        pass  # quiet; docker logs stay clean


def main():
    threading.Thread(target=_poll_stocks, daemon=True).start()
    threading.Thread(target=_poll_weather, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), _Handler)
    print("dash-bridge listening on 0.0.0.0:%d  watchlist=%s"
          % (PORT, ",".join(WATCHLIST)), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
