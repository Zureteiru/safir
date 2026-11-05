#!/usr/bin/env python3
"""
tools/fetch_assets.py - downloads the large inputs Safir is built from, into .cache/.

Nothing here is committed to git; every file is checked against a pinned SHA-256.

  .cache/models/VOCR6.2_S_w8.vocr     Small OCR model  (bee-san/vita-vn-ocr v0.5.0)
  .cache/models/VOCR6.2_N_w8g2.vocr   Normal OCR model (bee-san/vita-vn-ocr v0.5.0)
  .cache/JMdict_english.zip           JMdict in Yomitan format (yomidevs/jmdict-yomitan)

The OCR models are for NON-COMMERCIAL use only (see their attribution in the
vita-vn-ocr release). JMdict is (c) the Electronic Dictionary Research and Development
Group, CC BY-SA 4.0.

Usage: python3 tools/fetch_assets.py          (from the repo root; skips files already present)
"""

import hashlib
import os
import sys
import urllib.request

VOCR = "https://github.com/bee-san/vita-vn-ocr/releases/download/v0.5.0/"
JMDICT = "https://github.com/yomidevs/jmdict-yomitan/releases/download/2026-11-05/"

ASSETS = [
    (".cache/models/VOCR6.2_S_w8.vocr", VOCR + "VOCR6.2_S_w8.vocr",
     "864bbd196d5bc5d3a7bd7918cf7274b3503dfe65d440017ef0781ea7e7ba37d2"),
    (".cache/models/VOCR6.2_N_w8g2.vocr", VOCR + "VOCR6.2_N_w8g2.vocr",
     "107cf084cc0c1a3a61ce4c3f9b9cf6b31d6dff80f0a1bdb94afd3f7ee9ae3bc9"),
    (".cache/JMdict_english.zip", JMDICT + "JMdict_english.zip",
     "bb27b6b1e86ad86abf658dee09aeabe169674feb94213cbf112db23f6baa2ea7"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ok = True
    for rel, url, want in ASSETS:
        path = os.path.join(root, rel)
        if os.path.exists(path) and sha256(path) == want:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"downloading {url}")
        tmp = path + ".part"
        urllib.request.urlretrieve(url, tmp)
        got = sha256(tmp)
        if got != want:
            os.remove(tmp)
            print(f"ERROR: {rel}: SHA-256 {got}, expected {want}", file=sys.stderr)
            ok = False
            continue
        os.replace(tmp, path)
        print(f"ok: {rel}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
