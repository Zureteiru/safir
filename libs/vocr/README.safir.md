# Vendored: bee-san/vita-vn-ocr runtime

Files `vocr.c`, `vocr.h`, `vocr_lines.c`, `vocr_lines.h` are copied unmodified from
https://github.com/bee-san/vita-vn-ocr at commit `d408fe3` (`runtime/`).

License: GNU GPL v3 (see `LICENSE` in this folder). Because safir links this
code into `safir.ovl`, the combined overlay is distributed under GPLv3.

- `vocr.c`: CTC line recognizer that runs `VOCR6.2_S_w8.vocr` in place (no malloc).
- `vocr_lines.c`: polarity-agnostic dialogue line finder.

Build flags: C99 semantics with `-ffp-contract=off` (keeps NEON/scalar/host results
bit-identical, see upstream `runtime/README.md`).
