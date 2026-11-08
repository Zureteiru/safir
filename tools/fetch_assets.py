#!/usr/bin/env python3
"""
tools/fetch_assets.py - downloads the large inputs Safir is built from, into .cache/.

Nothing here is committed to git; every file but the Jiten list is checked against a
pinned SHA-256.

  .cache/models/VOCR6.2_S_w8.vocr     Small OCR model  (bee-san/vita-vn-ocr v0.5.0)
  .cache/models/VOCR6.2_N_w8g2.vocr   Normal OCR model (bee-san/vita-vn-ocr v0.5.0)
  .cache/models/textspot8_t64n8c16_w8.vtsp  Text line spotter (bee-san/vita-vn-ocr v0.3.10-textspot8.1)
  .cache/JMdict_english.zip           JMdict in Yomitan format (yomidevs/jmdict-yomitan)
  .cache/jiten_freq_global.zip        Jiten frequency list (jiten.moe): ranks the JMdict entries

The OCR models are for NON-COMMERCIAL use only (see their attribution in the
vita-vn-ocr release). JMdict is (c) the Electronic Dictionary Research and Development
Group, CC BY-SA 4.0. The Jiten frequency list is by Jiten (https://jiten.moe), CC BY-SA 4.0.
Jiten's download URL always serves the newest list, so it cannot be pinned: it is
downloaded once and kept (delete it to fetch a newer one).

Usage: python3 tools/fetch_assets.py          (from the repo root; skips files already present)
"""

import hashlib
import os
import sys
import urllib.request
import zipfile

VOCR = "https://github.com/bee-san/vita-vn-ocr/releases/download/v0.5.0/"
JMDICT = "https://github.com/yomidevs/jmdict-yomitan/releases/download/2026-11-05/"

ASSETS = [
    (".cache/models/VOCR6.2_S_w8.vocr", VOCR + "VOCR6.2_S_w8.vocr",
     "864bbd196d5bc5d3a7bd7918cf7274b3503dfe65d440017ef0781ea7e7ba37d2"),
    (".cache/models/VOCR6.2_N_w8g2.vocr", VOCR + "VOCR6.2_N_w8g2.vocr",
     "107cf084cc0c1a3a61ce4c3f9b9cf6b31d6dff80f0a1bdb94afd3f7ee9ae3bc9"),
    (".cache/models/textspot8_t64n8c16_w8.vtsp",
     "https://github.com/bee-san/vita-vn-ocr/releases/download/v0.3.10-textspot8.1/textspot8_t64n8c16_w8.vtsp",
     "e129d41adfa02c658a7d13d6f98064e6ab6ce9a2bc8c7a56da700a21357b5e1c"),
    (".cache/JMdict_english.zip", JMDICT + "JMdict_english.zip",
     "bb27b6b1e86ad86abf658dee09aeabe169674feb94213cbf112db23f6baa2ea7"),
    # Newest list, no pin (Jiten 26-10-02 was 85aab4fb7641a908c2560f30286c22e39448623dbcde8a592fbb86bcd8a272a8).
    (".cache/jiten_freq_global.zip", "https://api.jiten.moe/api/frequency-list/download", None),
]


def is_yomitan_zip(path):
    try:
        with zipfile.ZipFile(path) as z:
            return "index.json" in z.namelist()
    except zipfile.BadZipFile:
        return False


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
        if os.path.exists(path) and (want is None or sha256(path) == want):
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"downloading {url}")
        tmp = path + ".part"
        urllib.request.urlretrieve(url, tmp)
        got = sha256(tmp)
        if want is None:
            if not is_yomitan_zip(tmp):
                os.remove(tmp)
                print(f"ERROR: {rel}: not a Yomitan dictionary zip", file=sys.stderr)
                ok = False
                continue
        elif got != want:
            os.remove(tmp)
            print(f"ERROR: {rel}: SHA-256 {got}, expected {want}", file=sys.stderr)
            ok = False
            continue
        os.replace(tmp, path)
        print(f"ok: {rel}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
