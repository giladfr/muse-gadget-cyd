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

"""ota_push.c (firmware pushed in chunks over the Muse session) against fakes."""

from __future__ import annotations

import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class OtaPushTest(unittest.TestCase):
    def test_push_protocol(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "ota_push"
            compiled = subprocess.run(
                [*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "tests/ota_push_fakes"), "-I", str(ROOT / "main"),
                 str(ROOT / "main/ota_push.c"), str(ROOT / "tests/ota_push_harness.c"),
                 "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary), hashlib.sha256(b"abc").hexdigest()],
                                 capture_output=True, text=True,
                                 env={**os.environ, "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("PASSED", ran.stdout)


if __name__ == "__main__":
    unittest.main()
