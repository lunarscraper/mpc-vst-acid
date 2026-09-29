# Acid for MPC / Force

A TB-303-style acid bassline sequencer for the Akai MPC OS built-in plugin host. It generates two
bass lines, lets you blend between them, and plays them over MIDI into a synth track. Acid makes
no sound itself. It is a native VST2 plugin with a touchscreen skin and Q-Link control.

**Version 1.0.0** · GPL-3.0-only · by sd88me

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

## Use it
1. Add **Acid** to an instrument track from the plugin browser.
2. Acid plays through a MIDI output port named **Acid** (a second instance is **Acid 2**). On the track you want
   to hear it, set the MIDI input to that port and set **MIDI CH** to the channel that track listens on.
3. Press **GENERATE** on SEQ A, and on SEQ B if you want a second line, then start the transport.

The screen has three pages, and the Q-Links follow the page you are on.

### SEQ A and SEQ B
Each line is a separate sequencer with the same controls.

| Control | What it does |
|---|---|
| GENERATE / MUTATE | Make a new pattern, or vary the current one |
| DENSITY, ACCENT, SLIDE | How many steps play, and how many are accented or slid |
| OCTAVES | Note range of the pattern, 1 to 3 octaves |
| ALGO | Generation algorithm, 1 to 16 |
| LENGTH | Pattern length, 2 to 32 steps |
| GATE | Note length |
| OFFSET (A) / TUNE (B) | Start the pattern that many steps in on A. Transpose B by ±24 semitones |
| DIR | Forward, reverse or pendulum playback |
| REGEN | Regenerate automatically every 1 to 32 bars, or off |
| MIDI CH | MIDI channel the line plays on |
| BLEND A>B | The blend between the two lines, shared with the GLOBAL page |

### GLOBAL
| Control | What it does |
|---|---|
| SCALE, ROOT | Scale (12 choices, minor to chromatic) and root note used when generating |
| SWING, JITTER | Timing feel: swing 50 to 75%, and random timing variation |
| RESET ALL | Restart both patterns every 1, 2, 4 or 8 bars, or never |
| BLEND MODE | How A and B combine: LAYER, MORPH, SPLIT, FILL, XOR, LOCK or CHAIN |
| BLEND A>B | Position of the blend between the two lines |
| CV MODE | Sends both lines to a Force CV track for external CV/Gate hardware instead of a MIDI synth |

Settings are saved with your project.

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
