# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Serve the CYD dashboard simulator (esp32/tools/dash_sim) to a Muse.

``musegadget dash-sim`` registers this computer with the Muse as a dashboard
gadget offering only the CYD's ``dashboard.*`` commands, never
``system.run`` or file access, and forwards each one to a running
``dash_sim --listen PORT``. Button taps on the simulator's cards come back
through this service's local socket (``dash_sim --muse-socket``), like
``musegadget send-user-msg``.
"""

from __future__ import annotations

import json
import logging
import socket

from musegadget.executor import Account, error, ok

log = logging.getLogger(__name__)

DEFAULT_PORT = 8765
TIMEOUT_S = 10


def _s(description: str) -> dict:
    return {"type": "string", "description": description}


def _b(description: str) -> dict:
    return {"type": "boolean", "description": description}


# The commands and descriptions the CYD firmware registers
# (esp32/main/noise_control.cpp), so the Muse treats both alike.
COMMAND_SPECS = {
    "dashboard.card": {
        "description": (
            "Show your own screen on the dashboard (up to 4, after "
            "stocks/weather/calendar): text, rows with values, progress bars and "
            "sparklines, and buttons the user can tap to answer you. Kept until "
            "removed or ttl_s."
        ),
        "required": {"id": _s("Card id; the same id replaces it.")},
        "optional": {
            "json": _s(
                "All card fields as a JSON object (or string): {\"id\", \"title\", "
                "\"sub\", \"text\" (wrapped, ~4 lines), \"tone\" "
                "(up|down|accent|blue|dim), \"rows\": [{\"label\", \"value\", "
                "\"detail\", \"tone\", \"progress\" 0-100, \"spark\": [numbers]}] "
                "(up to 5), \"buttons\": [{\"id\", \"label\", \"say\"}] (up to 3; a "
                "tap sends \"say\" to you as a chat message), \"ttl_s\", \"show\": "
                "true to switch to it, \"remove\": true to delete}."
            ),
            "title": _s("Card title."),
            "text": _s("A paragraph, wrapped to ~4 lines."),
        },
    },
    "dashboard.notify": {
        "description": (
            "Show a short notification banner over the dashboard (20 s, or ttl_s; "
            "0 keeps it until tapped). Wakes a dimmed screen."
        ),
        "required": {"text": _s("Headline, one line.")},
        "optional": {
            "detail": _s("Second line."),
            "level": _s("info | success | warning | alert"),
            "card": _s("Card id a tap opens."),
        },
    },
    "dashboard.events": {
        "description": (
            "Button taps on your cards not yet collected (also sent to you as chat "
            "messages when possible)."
        ),
        "optional": {"peek": _b("Leave them queued.")},
    },
    "dashboard.data": {
        "description": (
            "Push data to a dashboard screen. The screen redraws if it is "
            "currently visible."
        ),
        "required": {
            "screen": _s("Dashboard screen to update: \"calendar\" or \"bridge\" "
                         "(full stocks/weather JSON)."),
            "json": _s(
                "JSON data for the screen, as a string or directly as an object. "
                "For calendar: {\"label\": \"Tuesday, Oct 6\", \"events\": "
                "[{\"time\": \"5:45 PM\", \"title\": \"...\"}]}. For bridge: the "
                "dash.json document; an empty stocks list keeps the last quotes."
            ),
        },
    },
    "dashboard.stocks": {
        "description": (
            "Live stock quotes the device fetches itself from Nasdaq (no API key) "
            "during market hours: set the watchlist (saved on the device), or with "
            "no params report status."
        ),
        "optional": {"symbols": _s("Watchlist, comma-separated, up to 8, e.g. AMD,NVDA,SPY.")},
    },
    "dashboard.calibrate": {
        "description": (
            "Show the touch calibration screen (tap three targets; saved on the "
            "device). Holding a finger on the screen for 5 seconds does the same."
        ),
        "optional": {"reset": _b("Forget the saved calibration instead.")},
    },
    "dashboard.debug": {
        "description": (
            "Return one-line diagnostics: touch (raw readings, pressure), backlight "
            "(light sensor, level), state and free heap."
        ),
    },
}


for _spec in COMMAND_SPECS.values():
    _spec.setdefault("required", {})
    _spec.setdefault("optional", {})


class DashSimExecutor:
    """Runs dashboard commands on a dash_sim listening on localhost."""

    def __init__(self, port: int = DEFAULT_PORT, host: str = "127.0.0.1") -> None:
        self.port = port
        self.host = host
        # The service reads the account only when it runs as root.
        self.account = Account.current()

    def run(self, command: str, params: dict, timeout_ms: int | None = None) -> dict:
        if command not in COMMAND_SPECS:
            return error(f"unsupported command: {command}")
        try:
            reply = self.request(command, params or {})
        except (OSError, ValueError) as exc:
            log.warning("%s: dash_sim unreachable: %s", command, exc)
            return error(f"the dashboard simulator isn't running on port {self.port} ({exc})")
        if not isinstance(reply, dict):
            return error("bad reply from the dashboard simulator")
        if not reply.pop("ok", False):
            return error(str(reply.get("error") or "failed"))
        return ok(reply)

    def request(self, command: str, params: dict) -> dict:
        line = json.dumps({"command": command, "params": params}) + "\n"
        with socket.create_connection((self.host, self.port), timeout=TIMEOUT_S) as sock:
            sock.sendall(line.encode())
            return json.loads(sock.makefile("rb").readline())
