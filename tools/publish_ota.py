"""Publish the built firmware as an over-the-air update.

Uploads build/led-proj.bin as ota/led-proj-<build>.bin and then ota/manifest.json to the
firmware repo (which must be PUBLIC: the board downloads from raw.githubusercontent.com without
any key). Boards fetch the manifest and install the file when its build number is higher than the
one they run.

Usage (the token is read from the environment and never stored):
    set GH_TOKEN=ghp_...                 (PowerShell: $env:GH_TOKEN = "ghp_...")
    python tools/publish_ota.py          # builds must already be done: idf.py build

Bump FW_BUILD in main/app_config.h before every release.
"""
import base64
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

REPO = os.environ.get("OTA_REPO", "EgozS/spotify-record-led-firmware")
BRANCH = "main"
BASE_URL = os.environ.get("OTA_BASE", "https://raw.githubusercontent.com/EgozS/spotify-record-led-firmware/main/")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "led-proj.bin")


def fail(msg):
    sys.exit("ERROR: " + msg)


token = os.environ.get("GH_TOKEN")
if not token:
    fail("set GH_TOKEN first (a GitHub token with write access to " + REPO + ")")

# --- build number
cfg = open(os.path.join(ROOT, "main", "app_config.h"), encoding="utf-8").read()
m = re.search(r"#define\s+FW_BUILD\s+(\d+)", cfg)
if not m:
    fail("FW_BUILD not found in main/app_config.h")
build = int(m.group(1))

# --- safety: never publish a firmware that may carry personal secrets
if os.path.exists(os.path.join(ROOT, "main", "secrets.h")):
    fail("main/secrets.h exists - it would be compiled into the firmware. Move it away and rebuild.")
if not os.path.exists(BIN):
    fail("build/led-proj.bin not found - run idf.py build first")
data = open(BIN, "rb").read()
if len(data) > 0x1A0000:
    fail("firmware is larger than the OTA slot")
for name in ("secrets.h", "secrets.h.bak"):
    path = os.path.join(ROOT, "main", name)
    if os.path.exists(path):
        for key, val in re.findall(r'#define\s+(\w+)\s+"([^"]+)"', open(path, encoding="utf-8").read()):
            if "CLIENT_ID" in key:
                continue  # the Spotify Client ID is public and belongs in the firmware
            b = val.encode()
            # Long values: any occurrence is suspicious. Short ones (e.g. a purely numeric password) also occur
            # inside ordinary lookup tables of the C library, so only a stand-alone NUL-delimited string counts.
            if (len(val) >= 16 and b in data) or (b"\x00" + b + b"\x00") in data:
                fail("the firmware contains the value of %s from main/%s. Do a clean rebuild without secrets.h."
                     % (key, name))

api = "https://api.github.com/repos/%s/contents/" % REPO


def call(method, path, body=None):
    req = urllib.request.Request(api + path + ("?ref=" + BRANCH if method == "GET" else ""), method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "led-disc-publish")
    if body is not None:
        req.add_header("Content-Type", "application/json")
        body = json.dumps(body).encode()
    try:
        for attempt in range(6):  # Python on Windows sometimes fails the certificate chain lookup; just retry
            try:
                with urllib.request.urlopen(req, body) as r:
                    return json.load(r)
            except urllib.error.HTTPError:
                raise
            except urllib.error.URLError as e:
                if attempt == 5:
                    raise
                time.sleep(3)
    except urllib.error.HTTPError as e:
        if e.code == 404 and method == "GET":
            return None
        fail("GitHub said %d for %s %s: %s" % (e.code, method, path, e.read().decode()[:300]))


def put(path, content, message):
    existing = call("GET", path)
    body = {"message": message, "content": base64.b64encode(content).decode(), "branch": BRANCH}
    if existing and isinstance(existing, dict) and "sha" in existing:
        body["sha"] = existing["sha"]
    call("PUT", path, body)


bin_name = "ota/led-proj-%d.bin" % build
manifest = {"build": build, "url": BASE_URL + bin_name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}

print("Publishing build %d (%d bytes) to %s ..." % (build, len(data), REPO))
put(bin_name, data, "Firmware build %d" % build)
put("ota/manifest.json", (json.dumps(manifest, indent=2) + "\n").encode(), "Release build %d" % build)
print("Done. Boards will pick up build %d on their next check (raw.githubusercontent.com caches for about 5 minutes)." % build)
print("Manifest: " + BASE_URL + "ota/manifest.json")
