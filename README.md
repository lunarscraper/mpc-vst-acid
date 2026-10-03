# Acid for MPC / Force

> **Requires MPC OS 3.x.** MPC OS 2.x needs further development: the touchscreen skins do not draw there yet (the page
> stays empty). See [MPC OS 2.x vs 3.x](https://github.com/sd88me/mpc-vst-plugins#mpc-os-2x-vs-3x) in the main repo.

A TB-303-style acid bassline sequencer for the Akai MPC OS built-in plugin host. It generates two
bass lines, lets you blend between them, and plays them over MIDI into a synth track. Acid makes
no sound itself. It is a native VST2 plugin with a touchscreen skin and Q-Link control.

<img width="640" height="400" alt="2026-09-29T124456312Z" src="https://github.com/user-attachments/assets/a9bac6d3-8b13-4715-95be-18ebab731cce" />

**Version 1.0.0** · GPL-3.0-only · by sd88me

## Use it
1. Add **Acid** to an instrument track from the plugin browser.
2. Acid plays through a MIDI output port named **Acid** (a second instance is **Acid 2**). On the track you want
   to hear it, set the MIDI input to that port and set **MIDI CH** to the channel that track listens on.
3. Press **GENERATE** on SEQ A, and on SEQ B if you want a second line, then start the transport.

The screen has three pages, and the Q-Links follow the page you are on.

### SEQ A and SEQ B
Each line is an independent sequencer of 2 to 32 steps. Both free-run from the MPC transport. The controls are the same
on both pages.

| Control | What it does |
|---|---|
| GENERATE | Re-roll the whole pattern from a new random seed. DENSITY, ACCENT, SLIDE, OCTAVES and ALGO are read at this moment, so they shape the *next* pattern, not the one playing |
| MUTATE | Nudge about 25% of the steps in place (rest/note, pitch), keeping the pattern recognisable. Repeat it to keep evolving |
| DENSITY | Chance that a step is a note rather than a rest |
| ACCENT | Chance that a note is accented (louder) |
| SLIDE | Chance that a note slides into the next step, 303-style. A slide into a rest becomes a plain note |
| OCTAVES | How many octaves above the root the pitches can span, 1 to 3 |
| ALGO | 1 is the classic density/accent/slide model. Higher values blend in a second generator (non-repeating pitches, random-walk density, pyramid accents), up to 16 |
| LENGTH | Steps before the pattern loops, 2 to 32. It takes effect at once. GENERATE to fill a longer pattern with new steps |
| GATE | Note length as a fraction of a step. It is live, and held slides ignore it |
| OFFSET (A) / TUNE OFFSET (B) | OFFSET rotates which step plays without rewriting the pattern. TUNE OFFSET sets B's interval relative to A, ±24 semitones (+7 is a fifth above, -12 an octave below) |
| DIR | Forward, reverse, or pendulum (bounces off each end) |
| REGEN | Re-roll automatically every 1 to 32 bars, or off |
| MIDI CH | MIDI channel the line plays on |
| BLEND A>B | The same control as on GLOBAL |

DENSITY, ACCENT, SLIDE and OCTAVES only affect the next GENERATE or MUTATE.

### GLOBAL
| Control | What it does |
|---|---|
| SCALE | Scale both lines quantise to: Minor, Phrygian, Harmonic Minor, Minor Pentatonic, Dorian, Major, Phrygian Dominant, Locrian, Whole Tone, Hungarian Minor, Minor Blues or Chromatic (no scale) |
| ROOT | The key both lines play in, C to B |
| SWING | 16th-note swing from 50% (straight) to 75%, shared so the lines stay locked |
| JITTER | Chance per step of perturbing which step plays, never when: skip a step, repeat the last one, or jump to a random one. 0 is off |
| RESET ALL | Every 1, 2, 4 or 8 bars, snap both patterns back to step 1 together. Off lets patterns of different lengths drift as a polymeter |
| BLEND MODE | How A and B merge into one mono line, decided step by step (see below) |
| BLEND A>B | Sweeps from A to B, from -63 (A only) to +64 (B). How it acts depends on BLEND MODE |
| CV MODE | Sends both lines to a Force CV track for external CV/Gate hardware instead of a MIDI synth |

### Blend modes
Except in LAYER, A and B are merged into one mono line, so a synth on a single track plays one note at a time. The
other line is muted on each step, so notes never overlap. BLEND A>B sweeps in a fixed, evenly spread order, so the hand-over is
repeatable rather than random.

| Mode | Result | BLEND A>B |
|---|---|---|
| LAYER | Both lines play together as two voices, crossfaded by velocity. It is the only mode that isn't a single mono line | Fades the velocity from A to B |
| MORPH | Each step comes from A or B | -63 is A only, +64 is B only |
| SPLIT | Rhythm, slide and accent from A, pitch from B | -63 is A only, +64 is all pitches from B |
| FILL | A plays, and B fills A's rests | -63 is A alone, centre is the fill result, +64 is B alone |
| XOR | Plays only where exactly one line has a note: interlocking, syncopated | Same sweep as FILL |
| LOCK | Plays only where both lines have a note: sparse and tight | Same sweep as FILL |
| CHAIN | Call and response: A plays a full pass, then B answers with a full pass | Sets the pass ratio. Centre is one pass each, and the ends give one side up to 8:1 |

Settings are saved with your project.

## What you need
- A first-generation MPC OS standalone device (32-bit ARM): Force, MPC Live / Live II, One, X or Key 61.
  Tested on a Force. Newer models are untested.
- **Root SSH access** to the device. Stock MPC OS doesn't offer it, so this is for modded units.
- Installing plugins this way is unofficial. Back up first and use it at your own risk.

## Install
1. Download `Acid-<version>-mpc-armv7.zip` from the
   [Releases page](https://github.com/sd88me/mpc-vst-acid/releases) and unzip it.
2. Copy the folder to the device: `scp -r Acid-1.0.0 root@<device-ip>:/tmp/`
3. Run the installer: `ssh root@<device-ip> sh /tmp/Acid-1.0.0/install.sh`

The installer **stops MPC** (save your project first), copies the plugin and skin, backs up
`MPC.settings`, registers the plugin and restarts MPC. Run it again to upgrade in place. Add `-y` to skip
the confirmation. `uninstall.sh` in the same folder removes it. `INSTALL.md` in the zip covers
installing by hand.


## Building from source
```
vst/build.sh     # armhf build in Docker: vst/build/acid.so, the skin, pluginlist-entry.xml
vst/test.sh      # offline x86 host test under ASan/UBSan; must print OK / PASSED
```
Both need `sd88me/mpc-vst-plugins` checked out next to this repo (`../mpc-vst`) or `MPC_VST=/path` set, for the
shared skin tools. Releases are built by the "VST release (draft)" GitHub Actions workflow using
mpc-vst-plugins' shared `vst-release.yml`.

- `src/`: the generation engine (`acid_core.c/.h`), vendored from `sd88me/force-acid` (see `src/VENDORED.md`)
- `vst/`: the plugin (`acid_vst.cpp`, MIDI out over ALSA seq), skin config (`vst.json`, `layout.conf`,
  `module.json`, `skin.css`, `fonts/`) and the build and test scripts

Acid is the MPC counterpart of the Force Shadow addon in `sd88me/force-acid` and uses the same engine.

## License
GPL-3.0-only. See `LICENSE`.
