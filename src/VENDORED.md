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
through the current Density/Accent/Slide/Octaves/Algo/Length/Scale values;
(3) get_param/set_param "a_dice"/"b_dice": the sequence as text, for the
project/preset chunk; re-vendor by diffing against force-acid's `src/` at a
newer commit and copying over both files if the engine changes.
(4) HOLD / SHIFT / VEL RAND per sequencer (set_param/get_param "hold", "shift", "velrand"), after the
Spektro NGEN ACDGEN's Hold Prob / Shift / Random Vel but written from scratch: an 11th die per step
(d_hold, its own PRNG stream so seeds still make the same patterns), dice string "D2" (still reads "D1",
giving it HOLD dice from the saved seed), ties through rests in emit_step_for_seq(), diatonic shift in
note_for_step(), VEL RAND on its own PRNG (vel_rng).
(5) Merged sd88me/mpc-vst-acid master up to db89ddc (2026-10-11): Mute, random pattern on a new instance,
"seed" get/set, and the song-position step path (0xF9 + "song_step"). The VST wrapper here keeps the
fork's PpqClock fix (24-PPQN pulses by absolute ppq index, exact-once) and never sends 0xF9, so the core
stays pulse-counted exactly as before; the song-position path is present but unused.
