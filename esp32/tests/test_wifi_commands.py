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

"""wifi.list / wifi.add / wifi.forget (app.c) against an in-memory saved list."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_ota import function_source  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "wifi_known.h"
#define TAG "test"
#define ESP_LOGI(tag, ...) ((void)(tag))

static char s_ui_wifi[48] = "Home";
static wifi_known_list_t s_list;
static int s_fail_save;

int wifi_known_load(wifi_known_list_t *list) { *list = s_list; return s_list.count; }
void wifi_known_wipe(wifi_known_list_t *list) { memset(list, 0, sizeof(*list)); }
bool wifi_known_remember(const char *ssid, const char *pass, int hidden) {
    if (s_fail_save) return false;
    wifi_known_list_t next = {0};
    snprintf(next.nets[0].ssid, sizeof(next.nets[0].ssid), "%s", ssid);
    snprintf(next.nets[0].password, sizeof(next.nets[0].password), "%s", pass);
    next.nets[0].hidden = hidden == 1;
    next.count = 1;
    for (int i = 0; i < s_list.count && next.count < WIFI_KNOWN_MAX; i++) {
        if (strcmp(s_list.nets[i].ssid, ssid)) next.nets[next.count++] = s_list.nets[i];
    }
    s_list = next;
    return true;
}
bool wifi_known_forget(const char *ssid) {
    wifi_known_list_t next = {0};
    for (int i = 0; i < s_list.count; i++) {
        if (strcmp(s_list.nets[i].ssid, ssid)) next.nets[next.count++] = s_list.nets[i];
    }
    s_list = next;
    return true;
}

static cJSON *command_error(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "code", code);
    cJSON_AddStringToObject(r, "message", message);
    return r;
}

@FUNCTIONS@

static char *run(const char *cmd, const char *params) {
    cJSON *p = params ? cJSON_Parse(params) : NULL;
    cJSON *r = wifi_command(cmd, p);
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    cJSON_Delete(p);
    return out;
}
#define EXPECT(cmd, params, text) do { \
    char *o = run(cmd, params); \
    if (!strstr(o, text)) { printf("FAIL %s %s: %s lacks %s\n", cmd, params ? params : "", o, text); exit(1); } \
    free(o); } while (0)
#define REJECT(cmd, params, text) do { \
    char *o = run(cmd, params); \
    if (strstr(o, text)) { printf("FAIL %s: %s has %s\n", cmd, o, text); exit(1); } \
    free(o); } while (0)

int main(void) {
    wifi_known_remember("Home", "homepass1", 0);
    EXPECT("wifi.list", NULL, "\"current\":\"Home\",\"saved\":[\"Home\"]");
    REJECT("wifi.list", NULL, "homepass1");  // passwords never leave
    // The only network can't be forgotten.
    EXPECT("wifi.forget", "{\"ssid\":\"Home\"}", "only network");
    // Bad input.
    EXPECT("wifi.add", "{}", "invalid_param");
    EXPECT("wifi.add", "{\"ssid\":\"Cafe\",\"password\":\"short\"}", "invalid_param");
    EXPECT("wifi.add", "{\"ssid\":\"012345678901234567890123456789012\"}", "invalid_param");
    // Add the new place's network: saved, the board stays put.
    EXPECT("wifi.add", "{\"ssid\":\"Office\",\"password\":\"officepass\"}",
           "\"saved\":[\"Office\",\"Home\"]");
    EXPECT("wifi.add", "{\"ssid\":\"Office\",\"password\":\"officepass\"}", "switches when");
    EXPECT("wifi.add", "{\"ssid\":\"Guest\"}", "\"saved\":[\"Guest\",\"Office\",\"Home\"]");
    assert(s_list.nets[1].password[0] && !s_list.nets[0].password[0]);
    // Now the current one can go.
    EXPECT("wifi.forget", "{\"ssid\":\"Home\"}", "\"saved\":[\"Guest\",\"Office\"]");
    EXPECT("wifi.forget", "{}", "missing_param");
    s_fail_save = 1;
    EXPECT("wifi.add", "{\"ssid\":\"X\"}", "storage_error");
    EXPECT("wifi.nope", "{}", "unsupported");
    puts("wifi commands passed");
    return 0;
}
'''


class WifiCommandsTest(unittest.TestCase):
    def test_wifi_commands(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler unavailable")
        if not (CJSON / "cJSON.c").exists():
            self.skipTest("cJSON source not found (run one idf.py build, or set CJSON_SOURCE_DIR)")
        app = (ROOT / "main/app.c").read_text()
        functions = "\n".join(function_source(app, sig) for sig in (
            "static int saved_network_count(", "static cJSON *wifi_command("))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "harness.c").write_text(HARNESS.replace("@FUNCTIONS@", functions))
            binary = tmp / "wifi"
            compiled = subprocess.run(
                [*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-g", "-fsanitize=address,undefined",
                 "-I", str(CJSON), "-I", str(ROOT / "main"),
                 str(tmp / "harness.c"), str(CJSON / "cJSON.c"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("wifi commands passed", ran.stdout)


if __name__ == "__main__":
    unittest.main()
