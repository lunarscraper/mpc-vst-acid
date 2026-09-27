# mpc-vst-acid

Acid (dual generative acid-bassline sequencer) ported as a native VST2
plugin for the Akai MPC OS built-in JUCE plugin host (Force, MPC Live/One/X/Key).

Sibling of the Force Shadow addon in `sd88me/force-acid` — same generation
engine (`src/acid_core.c`, vendored, see `src/VENDORED.md`), different
front end. This repo is standalone: no dependency on the force-acid repo at
build time, only on `sd88me/mpc-vst-plugins` (`../mpc-vst` by default, or
set `MPC_VST=/path/to/mpc-vst`) for the shared skin-rendering tools.

## Layout
- `src/` — vendored DSP/sequencer engine (`acid_core.c/.h`)
- `vst/` — the VST2 plugin (`acid_vst.cpp`, ALSA seq MIDI out), skin config
  (`vst.json`, `layout.conf`, `module.json`, `fonts/`), and build/test scripts

## Build
```
vst/build.sh
```
Docker-based; produces `vst/build/acid.so` (armhf), the skin folder,
and `pluginlist-entry.xml`. Requires `../mpc-vst` checked out next to this
repo (or `MPC_VST=...`).

## Test (offline, no device)
```
vst/test.sh
```
Builds and runs `vst/host_test.c` against `src/acid_core.c` + `acid_vst.cpp`
on x86 under ASan/UBSan: two plugin instances, param round-trip (float +
enum), a transport-driven tick pass, and VST chunk (get/setChunk) round-trip.
Must print `OK` / `PASSED` with no sanitizer errors.

## Deploy / device test
See `sd88me/mpc-vst-plugins`' `mpc-vst-plugin` skill (`.claude/skills/mpc-vst-plugin/SKILL.md`)
for staged deploy, MPC.settings registration, and `tools/bench.sh`.
