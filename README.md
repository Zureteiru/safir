# Safir

**A Japanese pop-up dictionary for visual novels on the Nintendo Switch.** Safir is a Tesla overlay: press **ZL+ZR** in a game, and it reads the Japanese text on screen and lets you look up every word in JMdict. Everything runs on the console itself, with no PC, phone or internet connection.

> **Status: v0.2, early and rough.** It works well on many visual novels, but it misreads or misses text in some games. Read [Known issues](#known-issues) before you install it.

> [!WARNING]
> **This project is written primarily by AI.** Nearly all of Safir's code, scripts and documentation were written by **Claude** (Anthropic's AI model, through Claude Code), directed and tested on real hardware by a human. The code has been tested on one console and checked with automated tests, but it hasn't had a thorough human code review. Read it with that in mind, and use it at your own risk.

Safir is inspired by [Rubi for Vita](https://github.com/bee-san/Rubi_for_vita), [meikipop](https://github.com/rtr46/meikipop) and [hoshidicts](https://github.com/Manhhao/hoshidicts). Its OCR models and recognizer come from [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr).

---

## What it does

1. **Screenshot.** You press ZL+ZR. Safir takes a screenshot of the game with the system's own screenshot service, without drawing anything over it first.
2. **Find the lines.** A small trained text spotter (vita-vn-ocr's textspot8) finds the text lines: first at half resolution over the whole screen, then again inside each text block at full resolution so small text stays sharp. The older hand-tuned line finder takes over if the spotter finds nothing.
3. **Read the lines.** It reads each line with a small Japanese OCR network (VOCR6.2, int8) on the console's CPU. A whole scan takes about 0.7–1.5 seconds. The network's output layer, about half of the model, is read from the SD card during the scan instead of being kept in memory.
4. **Split into words.** It splits the text into dictionary words: longest match first, with deinflection, so 食べさせられた finds 食べる. Deinflection uses Yomitan's rules, so the popup shows the same steps as Yomitan and Hoshi Reader (`causative › potential or passive › -た`).
5. **Look up.** It shows a popup next to the word you pick, with readings and English meanings from JMdict. When a word has several matches, the more common one comes first, using the Jiten frequency list (行った: 行く before 行う).

## Requirements

- A Switch running **Atmosphère** custom firmware. It was tested on firmware 21.0.0.
- **[nx-ovlloader](https://github.com/WerWolv/nx-ovlloader)** with a Tesla menu, or **[Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)**.
- **Overlay memory (nx-ovlloader heap):**
  - **4 MB:** works. Since v0.2 the Normal model needs about 0.7 MB instead of 1.2 MB, so `auto` can pick it at 4 MB when the popup leaves room; otherwise it uses Small.
  - **6 MB (recommended):** Normal and the widest popup always fit. You can set this in Ultrahand.
- About 30 MB free on the SD card, mostly for the 25 MB dictionary.

## Install

1. Download `safir-v0.2-sd.zip` from the [Releases](../../releases) page.
2. Unzip it onto the **root of your SD card**, so these files land in place:
   ```
   config/safir/config.ini
   switch/.overlays/safir.ovl
   switch/safir/dict.bin
   switch/safir/VOCR6.2_S_w8.vocr
   switch/safir/VOCR6.2_N_w8g2.vocr
   switch/safir/textspot8_t64n8c16_w8.vtsp
   ```
3. Restart the console, or reload overlays. "Safir" now appears in your overlay menu.
4. Open it once from the menu. After that, **ZL+ZR** opens it from any game.

## Controls

| Button | What it does |
|---|---|
| **ZL+ZR** (in a game) | Open Safir and scan the screen. The combo can be changed in `config.ini`. |
| **Two fingers on the touch screen** (in a game) | Same as ZL+ZR, for games that use ZL+ZR themselves. Handheld only; `touch_scan=0` turns it off. The game sees the touch too. |
| **D-pad / left stick ←→** | Previous / next word |
| **D-pad / left stick ↑↓** | Previous / next line |
| **A** | Show or hide the dictionary popup |
| **L / R**, right stick ←→ | Previous / next dictionary entry for the word |
| **ZL / ZR**, right stick ↑↓ (popup open) | Scroll the meanings |
| **Y** | Scan again |
| **B** | Close the popup; press again to go back to the game |
| **+** | Unload Safir |

## Settings: `sdmc:/config/safir/config.ini`

| Key | Values | Default | Meaning |
|---|---|---|---|
| `scan_combo` | button names joined by `+` (`L R ZL ZR LS RS DUP DDOWN DLEFT DRIGHT A B X Y MINUS PLUS`) | `ZL+ZR` | Buttons that open Safir and scan |
| `popup_width` | `auto` or `480`–`1024` | `auto` | Popup width in pixels. `auto` picks the widest that fits in memory. |
| `ocr_model` | `auto`, `small`, `normal` | `auto` | `auto` uses Normal when memory allows, else Small. `normal` forces it, and the popup gets narrower. |
| `touch_scan` | `0`, `1` | `1` | `1`: touching the screen with two fingers also opens Safir and scans. |
| `line_finder` | `textspot8`, `classic` | `textspot8` | `textspot8` is the trained spotter; `classic` is the older hand-tuned finder. |
| `share_popup_memory` | `0`, `1` | `0` | **Experimental.** `1` lets the OCR model use the popup's memory during a scan instead of its own, which saves 0.4–0.7 MB. The popup is hidden while scanning. |
| `save_scans` | `0`, `1` | `0` | Keeps each scan's screenshot as `scan_1.jpg`…`scan_4.jpg` in `sdmc:/switch/safir/`, useful for bug reports. |

**Per-game scan area:** `sdmc:/config/safir/games.ini` can limit scanning to one part of the screen for a game:

```ini
[0100XXXXXXXXXXXX]
x=100
y=500
w=1080
h=200
```

See also the per-game regions entry under Known issues.

## Known issues

These are real, current problems. Please report others in [Issues](../../issues), and attach `sdmc:/switch/safir/events.log`.

- **Some text is still missed or misread,** especially small text over busy artwork and unusual message boxes. v0.2's trained line finder (textspot8) fixes the cases tested so far, but it's new on the Switch. If it does worse in a game, `line_finder=classic` brings back the old finder.
- **Reopening with ZL+ZR sometimes showed the previous scan's text.** The cause was the system's screenshot service returning an old picture, for example on the first scan after switching games. v0.2 retakes an identical screenshot from another screen layer; whether that always gets a fresh picture is still being checked. If it happens, press **Y** to rescan, and please send `events.log`.
- **Horizontal text only.** Vertical (tategaki) text isn't supported.
- **Per-game regions (`games.ini`) may not apply.** The running game's title ID isn't always detected. The overlay then scans the full screen, which works for most games.
- **At 4 MB with a wide popup, `auto` may still choose the Small model,** which makes about twice as many mistakes as Normal. `share_popup_memory=1` or a narrower `popup_width` leaves room for Normal.
- **Memory is tight at 4 MB.** Very wide popups combined with the Normal model can run out of memory. Safir reports it instead of crashing, and lowering `popup_width` helps.
- **English meanings only** (JMdict). No pitch accent and no custom dictionaries yet. The Jiten frequency list only sorts the results; ranks aren't shown.
- **Tested on one console and a handful of games.** Expect rough edges on other setups.
- **No furigana handling.** Ruby text above a line can be read as noise or skipped.

## Building from source

You need Docker (it provides the devkitPro Switch toolchain) and Python 3.

```bash
python tools/fetch_assets.py   # OCR models, JMdict and the Jiten frequency list into .cache/
```

On Windows, build the overlay and the SD-card layout with:

```powershell
powershell -ExecutionPolicy Bypass -File .\build_switch.ps1
```

Without PowerShell, run the same two Docker steps by hand:

```bash
docker build -f Dockerfile.switch -t safir .
docker run --rm -v "$(pwd)/dist:/dist" safir
```

The output goes to `dist/` (`safir.ovl`, `dict.bin`, the models) and, with the PowerShell script, to `sd_package/` (ready to copy to the SD card).

**Repository layout:**
- `core/`: OCR, dictionary and layout code, plain C/C++;
- `switch/`: the overlay itself;
- `libs/`: vendored vita-vn-ocr runtime, libtesla and picojpeg;
- `tools/`: asset download, dictionary packer, memory-budget check, SD packaging.

## Licences and credits

- **Safir** is released under the **GNU GPL v3** ([LICENSE](LICENSE)), because it links the GPLv3 vita-vn-ocr runtime.
- **[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr)** by bee-san provides the recognizer and text-spotter runtime (`libs/vocr`, GPLv3), the **VOCR6.2 OCR models** and the **textspot8 spotter model** (release v0.3.10-textspot8.1). The models are licensed for **non-commercial use only**, because of their training data. See the attribution files in its releases. Safir's copy of `vocr.c` adds one marked change, streaming the output layer from the file; the other runtime files are unmodified.
- **[libtesla](https://github.com/WerWolv/libtesla)** (GPLv2 or later; a modified copy is in `libs/libtesla`).
- **[picojpeg](https://github.com/richgel999/picojpeg)** by Rich Geldreich (public domain; modified copy in `libs/picojpeg`).
- **[JMdict](https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project)** by the Electronic Dictionary Research and Development Group, **CC BY-SA 4.0**. It's converted from the [jmdict-yomitan](https://github.com/yomidevs/jmdict-yomitan) export. `dict.bin` is a derived work under the same licence.
- **[Jiten](https://jiten.moe)** frequency list (`jiten_freq_global.zip`), **CC BY-SA 4.0**. `dict.bin` uses its ranks to order the results.
- **Deinflection rules** come from [Yomitan](https://github.com/yomidevs/yomitan)'s `japanese-transforms.js` (GPLv3), as [hoshidicts](https://github.com/Manhhao/hoshidicts) (GPLv3) builds them. They're in `core/dict/safir_deinflect_rules.h`.

Safir isn't affiliated with Nintendo or any game publisher.
