# Acid for MPC / Force

💬 Questions or feedback? Join the [Open MPC Discord](https://discord.gg/sRRysZSgu3).

> **MPC OS.** This release works on **MPC OS 3.x**. On MPC OS 2.x it loads and plays from the Q-Links, but its touchscreen
> page stays empty until a release with a compatible skin is published. The [catalog](https://sd88me.github.io/mpc-vst-plugins/)
> shows which MPC OS each release works on, and the installers warn before putting a 3.x-only plugin on a 2.x device.
> See [MPC OS 2.x vs 3.x](https://github.com/sd88me/mpc-vst-plugins#mpc-os-2x-vs-3x) in the main repo.

A TB-303-style acid bassline sequencer for the Akai MPC OS built-in plugin host. It generates two
bass lines, lets you blend between them, and plays them over MIDI into a synth track. Acid makes
no sound itself. It is a native VST2 plugin with a touchscreen skin and Q-Link control.

<img width="640" height="400" alt="2026-09-29T124456312Z" src="https://github.com/user-attachments/assets/a9bac6d3-8b13-4715-95be-18ebab731cce" />

**Version 1.2.0** · GPL-3.0-only · by sd88me (fork with presets, live knobs, HOLD/SHIFT/VEL RAND, the Acid FX insert variant and the EDIT and MOD pages from upstream 2.0)

## Use it
1. Add **Acid** to an instrument track from the plugin browser.
2. Acid plays through a MIDI output port named **Acid** (a second instance is **Acid 2**). On the track you want
   to hear it, set the MIDI input to that port and set **MIDI CH** to the channel that track listens on.
3. Press **GENERATE** on SEQ A, and on SEQ B if you want a second line, then start the transport.

The screen has six pages (SEQ A, EDIT A, SEQ B, EDIT B, MOD, GLOBAL), and the Q-Links follow the page you are on.

### SEQ A and SEQ B
Each line is an independent sequencer of 2 to 32 steps. Both follow the MPC transport through the fork's clock fix: every 24-PPQN pulse is taken from the host's song position by its absolute index and fed exactly once, so tempo changes, uneven audio blocks and loop wraps neither double nor lose a step. The controls are the same
on both pages.

| Control | What it does |
|---|---|
| GENERATE | Re-roll the whole pattern from a new random seed |
| MUTATE | Nudge about 25% of the steps in place (rest/note, pitch), keeping the pattern recognisable. Repeat it to keep evolving |
| DENSITY | Chance that a step is a note rather than a rest |
| ACCENT | Chance that a note is accented (louder) |
| SLIDE | Chance that a note slides into the next step, 303-style. A slide into a rest becomes a plain note |
| OCTAVES | How many octaves above the root the pitches can span, 1 to 5 |
| ALGO | 1 is the classic density/accent/slide model. Higher values blend in a second generator (non-repeating pitches, random-walk density, pyramid accents), up to 16 |
| LENGTH | Steps before the pattern loops, 2 to 32. It takes effect at once |
| GATE | Note length as a fraction of a step. It is live, and held slides ignore it |
| HOLD | Chance that a note is held on through the rests after it (up to three, so up to a quarter note) instead of stopping after GATE, the long notes typical of 303 lines. 0 is off. Live, like DENSITY |
| SHIFT | Moves the whole line by scale degrees, -7 to +7, so it stays in the scale (+7 in a seven-note scale is an octave up). With CHROMATIC it moves by semitones |
| VEL RAND | Random velocity per note, on top of ACCENT, up to ±48 at 100%. 0 is off (and CV MODE ignores it) |
| OFFSET (A) / TUNE OFFSET (B) | OFFSET rotates which step plays without rewriting the pattern. TUNE OFFSET sets B's interval relative to A, ±24 semitones (+7 is a fifth above, -12 an octave below) |
| DIR | Forward, reverse, or pendulum (bounces off each end) |
| REGEN | Re-roll automatically every 1 to 32 bars, or off |
| MIDI CH | MIDI channel the line plays on |
| BLEND A>B | The same control as on GLOBAL |

Every knob is live: DENSITY, ACCENT, SLIDE, HOLD, OCTAVES and ALGO reshape the pattern that is playing, and turning a
knob back brings the earlier pattern back. GENERATE and MUTATE change the pattern itself.

HOLD, SHIFT and VEL RAND follow the *Hold Prob*, *Shift* and *Random Vel* parameters of the Spektro Audio NGEN's
ACDGEN generator. They are written from scratch from the NGEN manual's descriptions; no NGEN code or data is used.

The Q-Links carry knobs and selectors only. GENERATE, MUTATE, LOAD, SAVE, MUTE and CV MODE are touchscreen-only (or
MIDI CC), so turning a Q-Link never fires a trigger by accident.

### EDIT A and EDIT B
Each line has an edit page next to its SEQ page (taken over from upstream sd88me/mpc-vst-acid 2.0), showing 16 steps
at a time. A yellow marker above the step numbers follows the playing step while the transport runs (on the bar you
are viewing). The STEPS 1-16 / STEPS 17-32 switch picks which bar of a longer pattern you see; GENERATE and MUTATE
are there too. The 16 Q-Links turn the 16 NOTE sliders. Per step:

- **ON** switches the step between a note and a rest. A rest keeps its note, so switching it on brings the note back.
- **NOTE** is a vertical slider in scale steps from the root (the white line): one scale octave below it, up to five
  above, so it covers OCTAVES 1–5. The number under the slider is the value (0 = root, 7 = an octave up in a
  seven-note scale). SHIFT and TUNE still move the whole line on top of it.
- **ACC** and **SLIDE**: on the edit page a step can be both, as on a 303 (an accented note that slides into the
  next). GENERATE still rolls them as before, so existing patterns sound the same.
- **LOCK** marks a step as hand-edited. Editing a step locks it. A locked step keeps its note, accent and slide when
  DENSITY, ACCENT, SLIDE, OCTAVES, ALGO, LENGTH, SCALE, MUTATE or an LFO reshape the pattern. Tap LOCK to hand the
  step back to the knobs; it takes the knobs' value again at once.

Locks are saved with the project and in the preset slots, and cleared by GENERATE and REGEN.

### MOD
Two LFOs, each with two modulation slots (taken over from upstream sd88me/mpc-vst-acid 2.0, adapted to this fork's
live knobs and OCTAVES 1–5). Per LFO:

| Control | What it does |
|---|---|
| SHAPE | Sine, triangle, saw, square or sample & hold |
| RATE | Free-running speed, 0.05 to 20 Hz (shown while SYNC is off) |
| SYNC / DIV | With SYNC on, one LFO cycle lasts 1/4 note up to 16 bars and is locked to the song position, so it starts at the same point every time the song plays from there |
| MOD 1 DEST / MOD 2 DEST | What the slot moves: DENSITY, ACCENT, SLIDE, OCTAVES, LENGTH, GATE or OFFSET on either line, or BLEND |
| AMOUNT | -100 to +100 per slot. +100 sweeps the whole range of the destination |

Your knob values are never changed: the LFOs are added on top while the sequence plays, so the knobs, the presets
and the saved project keep your own settings. Turning the AMOUNT back to 0 gives back exactly the pattern you had.
LFO settings are saved with the project and in the preset slots; a slot saved before 1.2.0 loads with both LFOs off.

The Q-Links on MOD carry SHAPE, RATE, DIV, both destinations and both amounts of each LFO; SYNC stays on the
touchscreen (or MIDI CC), like the other switches.

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
| MUTE | Silences Acid's notes while the sequence keeps running in time (also on both SEQ pages) |
| CONTROL CH | The MIDI channel Acid listens on for remote control (see *Steuerung per MIDI* below). OFF, 1–16, default 16 |

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

Settings and both sequences are saved with your project and with plugin presets, so a reloaded project or
preset plays the same lines again.

### Presets
Acid has 32 preset slots, shared by all projects. A slot holds every setting and both sequences.

- **PRESET** (GLOBAL page) picks a slot. Turning it only browses; nothing changes yet.
- **SAVE** writes the current state to the picked slot, overwriting it.
- **LOAD** loads the picked slot. An empty slot leaves everything as it is.
- A slot holds the sound, not MUTE or CONTROL CH: loading a slot never mutes Acid or changes its control channel.
- Acid and Acid FX share the slots, so a slot saved in one loads in the other.
- The slots are also the plugin's programs, so the PRESET list at the top of the plugin window shows them, and
  picking one there loads it.

The slots live in `acid_presets.txt` next to the plugin folder on the SD card. Copy that file to back them up.

## Acid FX (Insert-Effekt)

Die Force erlaubt nur 8 Plugin-Instrumente gleichzeitig. **Acid FX** ist derselbe Sequencer als Insert-Effekt:
gleiche Bedienung, gleiche Speicherplätze, belegt aber **keinen** Instrument-Platz. Das Audio der Spur geht
unverändert durch.

1. **Acid FX** als Insert auf irgendeinen Track legen, z. B. direkt auf den Synth-Track, den Acid spielen soll,
   oder auf den Master.
2. Acid FX hat einen eigenen MIDI-Ausgang **Acid FX** (eine zweite Instanz heißt **Acid FX 2**). Beim Synth-Track
   den MIDI-Input auf diesen Port stellen und **MIDI CH** auf den Kanal, auf dem der Track hört.
3. Einmalig: Preferences → MIDI → beim Port `Acid FX` **Track** einschalten, falls er dort nicht aktiv ist.
4. GENERATE drücken, Transport starten.

Acid und Acid FX lassen sich nebeneinander installieren (eigene UID, eigene `.so`, eigener Port).

## Steuerung per MIDI (beide Varianten)

Jede Acid-Instanz hat neben dem Ausgang einen Eingang **Control In**. Er verbindet sich selbst mit allen
Hardware-MIDI-Eingängen, also USB-Controller wie Faderfox oder Circuit Tracks und DIN-In, auch mit später
eingesteckten. Außerdem kann eine MIDI-Spur ihn als Output-Port wählen. Ausgewertet wird **nur der Kanal, der
unter CONTROL CH eingestellt ist** (GLOBAL-Seite, Voreinstellung 16, OFF = aus). So lassen sich mehrere
Sequencer-Plugins vom selben Controller steuern, jedes auf seinem eigenen Kanal.

- **Noten** auf dem Control-Kanal transponieren beide Linien live (C4 = keine Transposition).
- **CCs** auf dem Control-Kanal stellen Parameter ein. 0–127 läuft über den ganzen Bereich; Wählschalter
  springen direkt auf ihre Option. Tasten (Generate, Mutate, Load, Save) lösen aus, wenn der Wert über 64
  steigt, also einmal pro Druck.

| CC | SEQ A | CC | SEQ B |
|---|---|---|---|
| 20 | Generate | 40 | Generate |
| 21 | Mutate | 41 | Mutate |
| 22 | Density | 42 | Density |
| 23 | Accent | 43 | Accent |
| 24 | Slide | 44 | Slide |
| 25 | Octaves | 45 | Octaves |
| 26 | Algo | 46 | Algo |
| 27 | Length | 47 | Length |
| 28 | Gate | 48 | Gate |
| 29 | Offset | 49 | Offset |
| 30 | Dir | 50 | Dir |
| 31 | Regen | 51 | Regen |
| 32 | MIDI CH | 52 | MIDI CH |
| 33 | Hold | 53 | Hold |
| 34 | Shift | 54 | Shift |
| 35 | Vel Rand | 55 | Vel Rand |
| | | 56 | Tune Offset |

| CC | GLOBAL | CC | Presets |
|---|---|---|---|
| 70 | Scale | 100 | Preset-Platz |
| 71 | Root | 101 | Load |
| 72 | Swing | 102 | Save |
| 73 | Jitter | | |
| 74 | Reset All | | |
| 75 | Blend Mode | | |
| 76 | Blend A>B | | |
| 77 | CV Mode | | |
| 78 | Mute | | |

| CC | MOD LFO 1 | CC | MOD LFO 2 |
|---|---|---|---|
| 80 | Shape | 88 | Shape |
| 81 | Rate | 89 | Rate |
| 82 | Sync | 90 | Sync |
| 83 | Div | 91 | Div |
| 84 | Mod 1 Dest | 92 | Mod 1 Dest |
| 85 | Mod 1 Amount | 93 | Mod 1 Amount |
| 86 | Mod 2 Dest | 94 | Mod 2 Dest |
| 87 | Mod 2 Amount | 95 | Mod 2 Amount |

Bei den Amount-CCs ist 64 ≈ 0 (keine Modulation), 0 = −100, 127 = +100.

Im Log (`/tmp/acid_vst.log` bzw. `/tmp/acid_fx_vst.log`) steht alle 5 s `control ch N: X cc seen, Y used`.
„seen" sind alle CCs, die ankommen, „used" die auf dem eingestellten Kanal mit gültiger Nummer. Damit lässt sich
prüfen, ob der Controller überhaupt ankommt.

## What you need
- A first-generation MPC OS standalone device (32-bit ARM): Force, MPC Live / Live II, One, X or Key 61.
  Tested on a Force. Newer models are untested.
- **Root SSH access** to the device. Stock MPC OS doesn't offer it, so this is for modded units.
- Installing plugins this way is unofficial. Back up first and use it at your own risk.

## Install
Acid FX installs the same way from its own zip, `Acid FX-<version>-mpc-armv7.zip` (workflow "VST release FX (draft)").

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
vst-fx/build.sh  # the same for Acid FX: vst-fx/build/acid_fx.so (insert effect)
vst-fx/test.sh   # its host test (also checks the audio pass-through)
```
Both need `sd88me/mpc-vst-plugins` checked out next to this repo (`../mpc-vst`) or `MPC_VST=/path` set, for the
shared skin tools. Releases are built by the "VST release (draft)" GitHub Actions workflow using
mpc-vst-plugins' shared `vst-release.yml`.

- `src/`: the generation engine (`acid_core.c/.h`), vendored from `sd88me/force-acid` (see `src/VENDORED.md`)
- `vst/`: the plugin (`acid_vst.cpp`, MIDI out over ALSA seq), skin config (`vst.json`, `layout.conf`,
  `module.json`, `skin.css`, `fonts/`) and the build and test scripts
- `vst-fx/`: Acid FX, only its own `vst.json` (`"effect": true`, own UID/name/.so) and build/test scripts; the
  source, parameters and skin come from `vst/`

Acid is the MPC counterpart of the Force Shadow addon in `sd88me/force-acid` and uses the same engine.

## License
GPL-3.0-only. See `LICENSE`.
