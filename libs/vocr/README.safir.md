# Vendored: bee-san/vita-vn-ocr runtime

Files `vocr.c`, `vocr.h`, `vocr_lines.c`, `vocr_lines.h` are copied from
https://github.com/bee-san/vita-vn-ocr at commit `d408fe3` (`runtime/`).
`vocr_lines.*` are unmodified. `vocr.c` / `vocr.h` carry one safir change (marked `safir:`),
streaming the output head from the file instead of keeping it in RAM:

- `vocr_head_span` + `vocr_load_split`: load a blob without the head's arrays.
- `vocr_extra.no_head`: `vocr_run` stops before the head and hands out its int16 input.
- `vocr_head_block` + `vocr_head_decode`: the head over caller-supplied rows, then greedy CTC.
  Bit-identical to the in-memory W8I head (tests/test_ocr_real.cpp, stream parity).

License: GNU GPL v3 (see `LICENSE` in this folder). Because safir links this
code into `safir.ovl`, the combined overlay is distributed under GPLv3.

- `vocr.c`: CTC line recognizer that runs `VOCR6.2_S_w8.vocr` in place (no malloc).
- `vocr_lines.c`: polarity-agnostic dialogue line finder.

Build flags: C99 semantics with `-ffp-contract=off` (keeps NEON/scalar/host results
bit-identical, see upstream `runtime/README.md`).

The files `vocr_textspot6.c`, `vocr_textspot6.h`, `vocr_textspot.h`, `vocr_lines_rows.c` and
`vocr_lines_rows.h` are copied unmodified from https://github.com/bee-san/vita-vn-ocr at commit
`05672d2` (`runtime/`) for the textspot8 line spotter. Its model blob
`textspot8_t64n8c16_w8.vtsp` comes from release `v0.3.10-textspot8.1`.
