#!/usr/bin/env python3
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

"""Push a firmware image to a CYD dashboard over the Muse session, in chunks.

For boards without the RAM for the HTTPS download device.ota needs. Runs
wherever device commands can be called from code (on the Muse VM):

    from ota_push import push
    push("cyd-dashboard-v1.6.0.bin", invoke)

where invoke(command, params) calls the device command and returns its
payload dict, raising on failure (with the payload, when there is one, as
exc.payload). Or from a shell, with any program that invokes a device command:

    ota_push.py cyd-dashboard-v1.6.0.bin --invoke 'my-invoke {command} {params_file}'

That program gets the params as a JSON file and must print the payload as
JSON (exit status non-zero on failure). ~450 chunks of 4 KB for a 1.8 MB
image; the board resumes from where it got to if the push is interrupted.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from typing import Callable

Invoke = Callable[[str, dict], dict]


class PushFailed(Exception):
    pass


def push(image: bytes | str, invoke: Invoke, retries: int = 5,
         log: Callable[[str], None] = print) -> str:
    """Send `image` (bytes or a path); returns the version the board installed."""
    if isinstance(image, str):
        with open(image, "rb") as f:
            image = f.read()
    size, sha = len(image), hashlib.sha256(image).hexdigest()

    def call(command: str, params: dict) -> dict:
        last = None
        for attempt in range(retries):
            try:
                return invoke(command, params)
            except Exception as exc:  # the transport, or the board refusing
                payload = getattr(exc, "payload", None) or {}
                if command == "ota.write" and "next" in payload:
                    return {"next": payload["next"], "refused": str(exc)}
                last = exc
                time.sleep(min(2 ** attempt, 10))
        raise PushFailed(f"{command} failed: {last}")

    begin = call("ota.begin", {"size": size, "sha256": sha})
    nxt, chunk = int(begin["next"]), int(begin.get("chunk_max", 4096))
    log(f"pushing {size} bytes in {chunk}-byte chunks from {nxt}")
    started, last_log = time.monotonic(), 0
    while nxt < size:
        data = base64.b64encode(image[nxt:nxt + chunk]).decode()
        reply = call("ota.write", {"offset": nxt, "data": data})
        got = int(reply["next"])
        if "refused" in reply and got == nxt:
            # Refused where we stand (e.g. the board abandoned the update):
            # begin again, which resumes or restarts.
            got = int(call("ota.begin", {"size": size, "sha256": sha})["next"])
        nxt = got
        if time.monotonic() - last_log > 10:
            last_log = time.monotonic()
            rate = nxt / max(time.monotonic() - started, 0.1) / 1024
            log(f"{nxt * 100 // size}% ({rate:.1f} KB/s)")
    done = call("ota.finish", {})
    version = done.get("version", "")
    log(f"board verified {version or 'the image'} and is rebooting")
    return version


def shell_invoke(template: str) -> Invoke:
    def invoke(command: str, params: dict) -> dict:
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(params, f)
            path = f.name
        try:
            argv = [a.format(command=command, params_file=path)
                    for a in shlex.split(template)]
            run = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        finally:
            os.unlink(path)
        try:
            payload = json.loads(run.stdout or "{}")
        except json.JSONDecodeError:
            payload = {}
        if run.returncode != 0:
            exc = RuntimeError(run.stderr.strip() or f"{command} failed")
            exc.payload = payload
            raise exc
        return payload
    return invoke


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image")
    ap.add_argument("--invoke", required=True,
                    help="command template with {command} and {params_file}")
    args = ap.parse_args()
    try:
        push(args.image, shell_invoke(args.invoke))
    except PushFailed as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
