# Vendored: acid_core.c / acid_core.h

Source: `sd88me/force-acid`, `src/acid_core.c` + `src/acid_core.h`
Vendored at commit: `e40708729fb9020a6469d46aaf0ee8153d72be73` (2026-09-26)
License: GPLv3 (same author, same license as this repo — see `LICENSE`).

This is the shared DSP/sequencer engine also used by the Force Shadow addon
(force-acid repo, `addon/`). It is vendored here rather than built against a
submodule so this VST port stays self-contained. Local changes against the
source commit above: (1) Octaves A/B range widened from 1..3 to 1..5 and rounded
instead of truncated (set_param "octaves" and the embedded chain_params);
(2) live knobs: Generate/Mutate roll per-step dice, derive_pattern() reads them
through the current Density/Accent/Slide/Octaves/Algo/Length/Scale values; re-vendor by diffing against force-acid's `src/` at a
newer commit and copying over both files if the engine changes.
