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

from __future__ import annotations

import json
import re
import socket
import threading
from pathlib import Path

from musegadget import cli, dash_sim, executor
from musegadget.service import Service

SRC = Path(__file__).resolve().parents[1] / "src" / "musegadget"


def _fake_sim(reply: dict) -> tuple[int, list]:
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    seen: list = []

    def serve():
        conn, _ = srv.accept()
        with conn, srv:
            seen.append(json.loads(conn.makefile("rb").readline()))
            conn.sendall(json.dumps(reply).encode() + b"\n")

    threading.Thread(target=serve, daemon=True).start()
    return srv.getsockname()[1], seen


def test_commands_are_forwarded_to_the_simulator():
    port, seen = _fake_sim({"ok": True, "status": "AMD 2/2"})
    result = dash_sim.DashSimExecutor(port).run("dashboard.stocks", {"symbols": "AMD"})
    assert result == {"ok": True, "payload": {"status": "AMD 2/2"}}
    assert seen == [{"command": "dashboard.stocks", "params": {"symbols": "AMD"}}]


def test_simulator_refusals_become_errors():
    port, _ = _fake_sim({"ok": False, "error": "id is required"})
    assert dash_sim.DashSimExecutor(port).run("dashboard.card", {}) == executor.error("id is required")


def test_only_dashboard_commands_are_offered():
    assert set(dash_sim.COMMAND_SPECS) == {
        "dashboard.card", "dashboard.notify", "dashboard.events", "dashboard.data",
        "dashboard.stocks", "dashboard.calibrate", "dashboard.debug",
    }
    for spec in dash_sim.COMMAND_SPECS.values():
        assert set(spec) == {"description", "required", "optional"}
    result = dash_sim.DashSimExecutor(1).run("system.run", {"command": "id"})
    assert result == executor.error("unsupported command: system.run")


def test_a_stopped_simulator_is_reported():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    result = dash_sim.DashSimExecutor(port).run("dashboard.debug", {})
    assert result["ok"] is False and "isn't running" in result["error"]


def test_service_registers_the_commands_it_is_given():
    service = Service(identity=None, executor=None, commands=dash_sim.COMMAND_SPECS)
    assert service.commands is dash_sim.COMMAND_SPECS
    assert Service(identity=None, executor=None).commands is executor.COMMAND_SPECS


def test_the_macos_peripheral_offers_the_same_service():
    # The backends need dbus and PyObjC, so compare their constants as text.
    def uuids(name: str) -> dict:
        text = (SRC / name).read_text()
        return dict(re.findall(r'^(\w+_UUID) = "([0-9a-f-]+)"', text, re.M))

    assert len(uuids("ble_server.py")) == 3
    assert uuids("ble_server_macos.py") == uuids("ble_server.py")


def test_dash_sim_command_line(monkeypatch):
    calls = []
    monkeypatch.setattr("musegadget.service.run_service", lambda *a, **k: calls.append((a, k)))
    monkeypatch.setattr(cli.identity, "load_or_create", lambda: "ident")
    monkeypatch.setattr(cli.config, "sdk_token", lambda: None)
    assert cli.main(["dash-sim", "--port", "9999"]) == 0
    (args, kwargs), = calls
    assert args[0] == "ident" and args[1].port == 9999
    assert kwargs["commands"] is dash_sim.COMMAND_SPECS
