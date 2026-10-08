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

"""tools/ota_push.py against a fake board that follows ota_push.c's rules."""

from __future__ import annotations

import base64
import hashlib
import random
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import ota_push  # noqa: E402


class Refused(Exception):
    def __init__(self, msg, payload):
        super().__init__(msg)
        self.payload = payload


class FakeBoard:
    CHUNK = 4096

    def __init__(self, flaky=0.0, abandon_at=None):
        self.active, self.flash, self.size, self.sha = False, b"", 0, ""
        self.flaky, self.abandon_at, self.calls = flaky, abandon_at, 0
        self.rng = random.Random(3)
        self.installed = None

    def invoke(self, command, params):
        self.calls += 1
        if self.rng.random() < self.flaky:
            raise TimeoutError("link dropped the reply")
        if command == "ota.begin":
            if not (self.active and params["size"] == self.size
                    and params["sha256"] == self.sha):
                self.active, self.flash = True, b""
                self.size, self.sha = params["size"], params["sha256"]
            return {"next": len(self.flash), "chunk_max": self.CHUNK}
        if command == "ota.write":
            nxt = len(self.flash)
            if not self.active:
                raise Refused("no update in progress", {"next": nxt})
            if params["offset"] != nxt:
                raise Refused("out of order", {"next": nxt})
            data = base64.b64decode(params["data"])
            assert len(data) <= self.CHUNK
            self.flash += data
            if self.abandon_at is not None and len(self.flash) >= self.abandon_at:
                self.abandon_at, self.active = None, False  # idle timeout
            return {"next": len(self.flash)}
        if command == "ota.finish":
            assert self.active and len(self.flash) == self.size
            assert hashlib.sha256(self.flash).hexdigest() == self.sha
            self.installed = self.flash
            return {"version": "1.6.0"}
        raise AssertionError(command)


class OtaPushSenderTest(unittest.TestCase):
    image = random.Random(1).randbytes(50_000)

    def push(self, board):
        return ota_push.push(self.image, board.invoke, log=lambda m: None)

    def test_clean_push(self):
        board = FakeBoard()
        self.assertEqual(self.push(board), "1.6.0")
        self.assertEqual(board.installed, self.image)

    def test_lost_replies_are_retried_and_resynced(self):
        # A write that landed but whose reply was lost is refused on retry;
        # the sender carries on from the board's `next`.
        board = FakeBoard(flaky=0.2)
        ota_push.time.sleep = lambda s: None
        self.assertEqual(self.push(board), "1.6.0")
        self.assertEqual(board.installed, self.image)

    def test_abandoned_update_starts_again(self):
        board = FakeBoard(abandon_at=20_000)
        self.assertEqual(self.push(board), "1.6.0")
        self.assertEqual(board.installed, self.image)

    def test_gives_up_on_a_dead_link(self):
        board = FakeBoard(flaky=1.0)
        ota_push.time.sleep = lambda s: None
        with self.assertRaises(ota_push.PushFailed):
            self.push(board)


if __name__ == "__main__":
    unittest.main()
