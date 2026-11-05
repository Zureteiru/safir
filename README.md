# Safir

**A Japanese pop-up dictionary for visual novels on the Nintendo Switch.** Safir is a Tesla overlay: press **ZL+ZR** in a game, and it reads the Japanese text on screen and lets you look up every word in JMdict. Everything runs on the console itself, with no PC, phone or internet connection.

> **Status: v0.1, early and rough.** It works well on many visual novels, but it misreads or misses text in some games. Read [Known issues](#known-issues) before you install it.

> [!WARNING]
> **This project is written primarily by AI.** Nearly all of Safir's code, scripts and documentation were written by **Claude** (Anthropic's AI model, through Claude Code), directed and tested on real hardware by a human. The code has been tested on one console and checked with automated tests, but it hasn't had a thorough human code review. Read it with that in mind, and use it at your own risk.

Safir is inspired by [Rubi for Vita](https://github.com/bee-san/Rubi_for_vita), [meikipop](https://github.com/rtr46/meikipop) and [hoshidicts](https://github.com/Manhhao/hoshidicts). Its OCR models and recognizer come from [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr).

---

## What it does

1. **Screenshot.** You press ZL+ZR. Safir takes a screenshot of the game with the system's own screenshot service, without drawing anything over it first.
2. **Find the lines.** It finds the text lines: first at half resolution over the whole screen, then each line again at full resolution so small text stays sharp.
3. **Read the lines.** It reads each line with a small Japanese OCR network (VOCR6.2, int8) on the console's CPU, which takes about half a second for a typical dialogue box.
4. **Split into words.** It splits the text into dictionary words: longest match first, with deinflection, so 食べさせられた finds 食べる.
5. **Look up.** It shows a popup next to the word you pick, with readings and English meanings from JMdict.

## Requirements

- A Switch running **Atmosphère** custom firmware. It was tested on firmware 21.0.0.
- **[nx-ovlloader](https://github.com/WerWolv/nx-ovlloader)** with a Tesla menu, or **[Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)**.
- **Overlay memory (nx-ovlloader heap):**
  - **4 MB:** works, with the Small OCR model.
  - **6 MB (recommended):** Safir picks the more accurate Normal model and the widest popup. You can set this in Ultrahand.
- About 30 MB free on the SD card, mostly for the 25 MB dictionary.

## Install

1. Download `safir-v0.1-sd.zip` from the [Releases](../../releases) page.
2. Unzip it onto the **root of your SD card**, so these files land in place:
   ```
   config/safir/config.ini
   switch/.overlays/safir.ovl
   switch/safir/dict.bin
   switch/safir/VOCR6.2_S_w8.vocr
   switch/safir/VOCR6.2_N_w8g2.vocr
   ```
3. Restart the console, or reload overlays. "Safir" now appears in your overlay menu.
4. Open it once from the menu. After that, **ZL+ZR** opens it from any game.

## Controls

| Button | What it does |
|---|---|
| **ZL+ZR** (in a game) | Open Safir and scan the screen. The combo can be changed in `config.ini`. |
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
| `ocr_model` | `auto`, `small`, `normal` | `auto` | `auto` uses Normal when memory allows (about 6 MB heap), else Small. `normal` forces it, and the popup gets narrower. |
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

- **Small text, text over busy artwork and unusual message boxes are sometimes missed or misread.** The line finder is hand-tuned on a few screenshots, so it fails on layouts it hasn't seen. The planned fix is to switch to a trained text spotter (vita-vn-ocr's textspot8) and judge every change against a collection of real screenshots.
- **Reopening with ZL+ZR sometimes shows the previous scan's text** instead of what's on screen. It happens rarely and seemingly at random, and it's under investigation. Press **Y** to rescan. If it happens to you, setting `save_scans=1` and sending the screenshots plus `events.log` helps a lot.
- **Horizontal text only.** Vertical (tategaki) text isn't supported.
- **Per-game regions (`games.ini`) may not apply.** The running game's title ID isn't always detected. The overlay then scans the full screen, which works for most games.
- **The Normal model needs about 6 MB of overlay memory.** At the default 4 MB, `auto` falls back to the Small model, which makes about twice as many mistakes.
- **Memory is tight at 4 MB.** Very wide popups combined with the Normal model can run out of memory. Safir reports it instead of crashing, and lowering `popup_width` helps.
- **English meanings only** (JMdict). No pitch accent, no frequency lists, no custom dictionaries yet.
- **Tested on one console and a handful of games.** Expect rough edges on other setups.
- **No furigana handling.** Ruby text above a line can be read as noise or skipped.

## Building from source

You need Docker (it provides the devkitPro Switch toolchain) and Python 3.

```bash
python tools/fetch_assets.py   # OCR models + JMdict into .cache/ (SHA-256 checked)
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
- **[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr)** by bee-san provides the recognizer runtime (`libs/vocr`, GPLv3) and the **VOCR6.2 OCR models**. The models are licensed for **non-commercial use only**, because of their training data. See the attribution files in their v0.5.0 release.
- **[libtesla](https://github.com/WerWolv/libtesla)** (GPLv2 or later; a modified copy is in `libs/libtesla`).
- **[picojpeg](https://github.com/richgel999/picojpeg)** by Rich Geldreich (public domain; modified copy in `libs/picojpeg`).
- **[JMdict](https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project)** by the Electronic Dictionary Research and Development Group, **CC BY-SA 4.0**. It's converted from the [jmdict-yomitan](https://github.com/yomidevs/jmdict-yomitan) export. `dict.bin` is a derived work under the same licence.

Safir isn't affiliated with Nintendo or any game publisher.
