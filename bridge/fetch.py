#!/usr/bin/env python3
"""One-shot fetch of the dashboard JSON (stocks + weather).

Reuses bridge.py's Nasdaq/Open-Meteo logic, prints the dash.json document
to stdout. Used by the push cron: the agent takes this output and sends it
to the CYD via dashboard.data screen="bridge".
"""
import concurrent.futures
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge

def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        quotes = [q for q in ex.map(bridge._fetch_quote, bridge.WATCHLIST) if q]
    order = {s: i for i, s in enumerate(bridge.WATCHLIST)}
    quotes.sort(key=lambda q: order.get(q["symbol"], 99))
    try:
        wx = bridge._fetch_weather()
    except Exception as e:
        print("weather fetch failed: %s" % e, file=sys.stderr)
        wx = None
    doc = {
        "stocks": quotes,
        "weather": wx,
        "updated": int(time.time()),
    }
    print(json.dumps(doc))

if __name__ == "__main__":
    main()
