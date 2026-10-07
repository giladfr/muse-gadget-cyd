#!/usr/bin/env python3
"""The Muse bridge end to end, without Bluetooth or a Muse.

Runs dash_sim --listen/--muse-socket headless, sends it Muse commands through
the SDK's real DashSimExecutor (what `musegadget dash-sim` runs), stands in for
musegadget's local socket, and checks that a tap on a card button arrives
there as the chat message.

    test_muse_bridge.py path/to/dash_sim
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../../../linux/src"))
from musegadget.dash_sim import COMMAND_SPECS, DashSimExecutor  # noqa: E402

SCRIPT = """\
sleep 2500
tap 80 202
sleep 1500
dashboard.events {"peek":true}
expect "delivered":true
quit
"""

failures = []


def check(what, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f": {detail}"))
    if not cond:
        failures.append(what)


def fake_musegadget(path, received):
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    srv.settimeout(15)

    def serve():
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        with conn:
            received.append(json.loads(conn.makefile("rb").readline()))
            conn.sendall(b'{"ok": true, "status": 200, "response": {"id": "m1"}}\n')

    threading.Thread(target=serve, daemon=True).start()
    return srv


def main():
    sim = sys.argv[1]
    tmp = tempfile.mkdtemp()
    sock_path = os.path.join(tmp, "mg.sock")
    script = os.path.join(tmp, "script.txt")
    with open(script, "w") as f:
        f.write(SCRIPT)
    received = []
    srv = fake_musegadget(sock_path, received)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen(
        [sim, "--headless", "--nvs", os.path.join(tmp, "nvs.txt"), "--listen", str(port),
         "--muse-socket", sock_path, "--script", script],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ex = DashSimExecutor(port)
    reply = {}
    for _ in range(50):  # until the listener is up
        reply = ex.run("dashboard.debug", {})
        if reply.get("ok"):
            break
        time.sleep(0.05)
    check("dashboard.debug answers", reply.get("ok") and "debug" in reply["payload"], reply)

    card = {"id": "standup", "json": {
        "id": "standup", "title": "Standup", "text": "Standup in 10 minutes. Join?",
        "buttons": [{"id": "join", "label": "Join", "say": "Join my standup call."}],
        "show": True}}
    reply = ex.run("dashboard.card", card)
    check("dashboard.card shows the card", reply == {"ok": True, "payload": {}}, reply)
    reply = ex.run("dashboard.card", {"json": {"title": "no id"}})
    check("a bad card is refused", reply.get("ok") is False and reply.get("error"), reply)
    reply = ex.run("dashboard.stocks", {})
    check("dashboard.stocks reports", reply.get("ok") and "status" in reply["payload"], reply)
    reply = ex.run("system.run", {"command": "id"})
    check("system.run is not offered", reply.get("ok") is False and "system.run" not in COMMAND_SPECS,
          reply)

    out, _ = proc.communicate(timeout=60)
    srv.close()
    check("dash_sim script passed", proc.returncode == 0, out[-1500:])
    check("the tap reached musegadget as a chat message",
          received == [{"message": "[Desk display] Join my standup call."}], received)
    check("Muse's answer was shown", "Muse answered 200" in out, out[-800:])
    reply = ex.run("dashboard.debug", {})
    check("a stopped simulator is reported", reply.get("ok") is False and "isn't running"
          in reply.get("error", ""), reply)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
