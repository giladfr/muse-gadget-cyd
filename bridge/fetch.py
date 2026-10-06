#!/usr/bin/env python3
"""One-shot fetch of the dashboard JSON (stocks + weather).

Reuses bridge.py's Nasdaq/Open-Meteo logic, prints the dash.json document
to stdout. Used by the push cron: the agent takes this output and sends it
to the CYD via dashboard.data screen="bridge".

A feed that fails is sent empty with a 0 timestamp; the board keeps showing
its last good data for that feed instead of blanking it.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge


def main():
    try:
        quotes = bridge.fetch_quotes()
    except Exception as e:
        print("stock fetch failed: %s" % e, file=sys.stderr)
        quotes = []
    stocks_updated = int(time.time()) if quotes else 0
    try:
        wx = bridge._fetch_weather()
        weather_updated = int(time.time())
    except Exception as e:
        print("weather fetch failed: %s" % e, file=sys.stderr)
        wx, weather_updated = None, 0
    doc = bridge.document(quotes, wx, stocks_updated, weather_updated)
    print(json.dumps(doc, separators=(",", ":")))


if __name__ == "__main__":
    main()
