# Vendored: acid_core.c / acid_core.h

Source: `sd88me/force-acid`, `src/acid_core.c` + `src/acid_core.h`
Vendored at commit: `e40708729fb9020a6469d46aaf0ee8153d72be73` (2026-09-26)
License: GPLv3 (same author, same license as this repo — see `LICENSE`).

This is the shared DSP/sequencer engine also used by the Force Shadow addon
(force-acid repo, `addon/`). It is vendored here rather than built against a
submodule so this VST port stays self-contained. Not modified from the
source commit above; re-vendor by diffing against force-acid's `src/` at a
newer commit and copying over both files if the engine changes.
