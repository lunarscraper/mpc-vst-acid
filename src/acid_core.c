/* Acid -- dual generative acid-bassline sequencer, slot MIDI FX for Schwung.
 *
 * Two independent 8..32-step generators (Seq A / Seq B) free-run off Move's
 * transport, sharing one Root/Scale. Each sequencer's own "Algo" knob (1-16)
 * blends a primary generator (density/accent/slide/octave probabilistic
 * model, ported from schwung-tb3po / the Phazerville TB_3PO applet) with a
 * secondary generator (Sting.amxd-inspired: urn-style non-repeating pitch
 * draw, a density-modulated bounded random walk for gate/rest, and a fixed
 * permutation ("VelPyra") accent shape instead of independent per-step
 * accent rolls). Algo=1 is 100% primary -- byte-for-byte the same generation
 * behaviour as tb3po's model. Algo=16 is mostly secondary.
 *
 * Because a MIDI FX slot forwards process_midi/tick output to exactly ONE
 * synth on ONE channel (chain_midi.c overwrites the channel byte with the
 * slot's recv channel regardless of what we send), Seq A and Seq B are not
 * routed to separate synths the way tb3po's two Tool slots are. Instead
 * both merge into the single output stream, mixed by a bipolar Blend knob
 * (-63..64) that crossfades the two by scaling each side's own velocity:
 * -63 = A only @127, 0 = both @100, +64 = B only @127.
 *
 * Blend doubles as Seq B's on/off -- at -63 it mutes B outright -- so the
 * knob that would otherwise be a B enable is Tune B instead: B's interval
 * from A in semitones (+/-24), layered on top of Root and live transpose.
 *
 * Swing (50-75%, MPC-style 16th) is shared by both sequencers off one
 * clock, so they always stay swung together regardless of Reset Both or
 * differing lengths -- see swing_delay_frac()/swing_gap_mult() for the
 * timing math.
 *
 * No banks, no undo, no persistence in this version -- Generate/Mutate only.
 * An incoming note-on transposes both sequencers live, relative to C4
 * (hybrid trigger model) -- the Root knob is left untouched, and Tune B's
 * interval rides along, so A and B stay locked at the interval you set.
 * Sequencing itself keeps running regardless of note input.
 *
 * ---------------------------------------------------------------------
 * Ported to the Akai Force (MockbaMod): verbatim from schwung-acid except
 * for one addition, marked FORCE-ONLY throughout. Move's chain host forces
 * every MIDI FX slot onto one output channel (see above) -- Force's host
 * (src/host_shim.cpp, a standalone RtMidi process, not a chain slot) has no
 * such restriction, so tb3po's original two-separate-outputs design becomes
 * possible again: each sequencer gets its own independently selectable
 * output channel (a_channel/b_channel). Blend is unchanged and still applies
 * on top -- splitting to two channels doesn't retire it, it just means the
 * two sides can *also* land on separate instrument tracks. Search
 * FORCE-ONLY for every touch point; see DESIGN.md for the rest of the
 * Move/Force delta list.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "acid_core.h"

#define MAX_STEPS   32
#define MIN_LENGTH  2
#define NUM_SEQS    2
#define SEQ_A       0
#define SEQ_B       1
#define OUT_CH      0   /* FORCE-ONLY: compile-time default for acid_seq_t.out_ch
                         * (0 = MIDI channel 1). Upstream's OUT_CH was a fixed
                         * channel baked into every emitted message; here it's
                         * only the calloc-matching default -- the live value
                         * per sequencer is out_ch (a_channel/b_channel). */
#define ACID_ANCHOR_NOTE 60  /* incoming note that means "no transpose" (C4, like tb3po-lite) */
#define ACID_MAX_TUNE 24     /* Seq B's interval from Seq A, +/- two octaves */
#define ACID_MAX_TRANSPOSE 48

typedef enum { STEP_REST = 0, STEP_NOTE = 1, STEP_ACCENT = 2, STEP_SLIDE = 3 } step_kind_t;

/* Scale degrees in semitones from root -- ported verbatim from tb3po. */
typedef struct { const char *name; int degrees[12]; int len; } scale_t;
static const scale_t SCALES[] = {
    { "Minor",     {0, 2, 3, 5, 7, 8, 10},         7 },
    { "Phrygian",  {0, 1, 3, 5, 7, 8, 10},         7 },
    { "HarmMinor", {0, 2, 3, 5, 7, 8, 11},         7 },
    { "MinPent",   {0, 3, 5, 7, 10, 0, 0},         5 },
    { "Dorian",    {0, 2, 3, 5, 7, 9, 10},         7 },
    { "Major",     {0, 2, 4, 5, 7, 9, 11},         7 },
    /* Appended for v1.1 -- a curated acid/techno-leaning set, not an attempt
     * at completeness. Order matters: these must stay at indices 6..11 so any
     * stored/automated `scale` value <= 5 keeps its meaning. note_for_step(),
     * gen_primary(), gen_secondary() and mutate_pattern() all read SCALES[]
     * generically, and NUM_SCALES is sizeof-derived, so nothing else changes
     * with the list. Chromatic (len 12) is the one that functionally answers
     * "scale defeat" without a second code path -- degrees[12]/len already
     * supports it. */
    { "PhrygDom",  {0, 1, 4, 5, 7, 8, 10},         7 },  /* Phrygian Dominant / Spanish */
    { "Locrian",   {0, 1, 3, 5, 6, 8, 10},         7 },
    { "WholeTone", {0, 2, 4, 6, 8, 10},            6 },
    { "HungMinor", {0, 2, 3, 6, 7, 8, 11},         7 },  /* Hungarian / Gypsy Minor */
    { "MinBlues",  {0, 3, 5, 6, 7, 10},            6 },
    { "Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12 }
};
#define NUM_SCALES ((int)(sizeof(SCALES) / sizeof(SCALES[0])))

/* Sting.amxd's "VelPyra" fixed permutation -- an evenly-spread-but-irregular
 * accent order (used by the secondary generator instead of independent
 * per-step accent rolls). Values 0-15, taken directly from the patch --
 * kept deliberately for now (v1: match/learn the reference's actual feel on
 * hardware before designing an original replacement for final release; see
 * the design doc and README credit for the license/attribution status of
 * this specific table). */
static const uint8_t VEL_PYRAMID[16] = {0, 14, 15, 6, 1, 2, 10, 3, 12, 13, 11, 5, 4, 8, 7, 9};

/* Bar-length lookup for the Reset Both control: index -> 16th-notes per bar
 * boundary (index 4 = "Off", handled separately, never indexes this). */
static const int BAR_STEPS[4] = {16, 32, 64, 128};

/* Auto Regen (Advanced page) bar-boundary lookup: index 1..6 -> 16th-notes per
 * automatic-regenerate boundary; index 0 = "Off", handled separately and
 * never indexes this. Kept separate from BAR_STEPS[] on purpose -- Auto Regen's
 * enum puts "Off" first and reaches further (up to 32 bars) than Reset Both. */
static const int AUTO_GEN_BAR_STEPS[6] = {16, 32, 64, 128, 256, 512};

/* FORCE-ONLY: fixed per-step duration for the Export dump player (see
 * acid_seq_t's dump_* fields), in milliseconds -- deliberately independent
 * of tempo/rate so exporting never needs the live transport running. 60ms
 * is short enough that a 32-step dump finishes in well under 2 seconds,
 * long enough that a MIDI listener can cleanly tell consecutive
 * note-on/off pairs apart. */
#define DUMP_STEP_MS 60

/* FORCE-ONLY: the dump player emits on a fixed channel rather than the
 * seq's own out_ch, so an export capture can't be corrupted by live
 * playback happening concurrently on the same channel (the whole point of
 * a separate dump state machine). Reuses host_shim.cpp's FEEDBACK_CHANNEL
 * (also 15, 0-based -- MIDI channel 16), which is already established as
 * a side-channel that never carries live note output, only CC feedback --
 * safe to also carry the dump's note-on/off/CC65 stream. */
#define DUMP_CHANNEL 15

typedef struct {
    /* PRNG -- xorshift32. `rng` is the live, ever-advancing generator state
     * (mutate consumes from wherever it currently sits); `seed` is the last
     * value a full Generate was seeded with, kept for reference/debugging. */
    uint32_t rng;
    uint32_t seed;

    /* Pattern */
    uint8_t steps[MAX_STEPS];
    uint8_t degrees[MAX_STEPS];
    uint8_t octaves[MAX_STEPS];
    int length;
    int position;

    /* Knobs */
    float density, accent, slide;
    int octave_range;   /* 1..3 */
    float gate;         /* 0.05..1.0, fraction of a step */
    int algo;           /* 1..16 */

    /* Advanced page -- per-sequencer playback modifiers. Offset is read-side
     * only (rotates which stored step plays, never touches steps[]/degrees[]/
     * octaves[]); Direction changes how `position` advances each tick. */
    int offset;         /* 0..length-1 rotation applied when reading a step */
    int dir;            /* 0=Fwd, 1=Rev, 2=Pendulum */
    int pendulum_fwd;   /* dir==2 only: 1 = currently travelling forward */

    /* Playback */
    int tune;           /* semitone offset from the shared key; Seq A is always
                         * 0, Seq B is the user's A->B interval. Applied on top
                         * of root + live_transpose, so MIDI transposition moves
                         * both sequencers and preserves the interval. */
    int out_ch;         /* FORCE-ONLY: 0-based output MIDI channel for this
                         * sequencer's note-on/off + slide CC65 stream
                         * (a_channel/b_channel). No Move equivalent. Defaults
                         * to 0 (channel 1) via calloc, matching the old
                         * single-channel behaviour until set otherwise. */
    int last_note_on;   /* -1 = none */
    int portamento_on;
    long gate_samples_remaining;

    /* FORCE-ONLY: independent per-seq Auto Regen, not a Move/upstream param
     * (upstream/original had one shared Auto Gen for both sequencers -- see
     * acid_inst_t's history / a_channel comment above for the same pattern).
     * idx: 0=Off, 1..6 -> {1,2,4,8,16,32} bars, own counter so A and B free-run
     * independently instead of sharing one bar-boundary. */
    int auto_gen_idx;
    long auto_gen_step_count;

    /* FORCE-ONLY: one-shot "Export as MIDI Clip" dump player -- entirely
     * separate from the live position/last_note_on/portamento_on state
     * above, so triggering a dump never disturbs whatever this sequencer
     * (or the other one) is currently playing live. Ticks on its own fixed,
     * tempo-independent cadence (DUMP_STEP_SAMPLES) driven straight off
     * acid_tick()'s own sample-rate clock, so it runs even while
     * t->running is 0 -- see process_dump_for_seq(). */
    int dump_active;            /* 1 while a dump is in progress */
    int dump_pos;               /* which step is about to fire, 0..length-1 */
    long dump_sample_accum;     /* accumulator toward the next dump step */
    int dump_last_note;         /* -1 = none; last dump note-on, for note-off */
    int dump_portamento_on;     /* separate from the live portamento_on */
} acid_seq_t;

typedef struct {
    acid_seq_t seq[NUM_SEQS];

    int root;             /* 0-11, knob/preset only -- never written by MIDI in */
    int live_transpose;   /* semitones from incoming notes, anchored at C4; kept
                           * separate from `root` so playing notes (or an echo of
                           * our own output) never moves the Root knob/field */
    int scale;            /* index into SCALES */
    int blend;             /* -63..64, bipolar velocity crossfade A<->B */
    int reset_bars_idx;   /* 0..3 -> {1,2,4,8} bars, 4 = Off */
    /* FORCE-ONLY: CV Mode -- retargets both sequencers' output for a Force CV
     * track feeding external CV/Gate/Accent/Slide hardware (e.g. a Behringer
     * TD-3-MO) instead of a MIDI synth. No Move equivalent. See
     * emit_step_for_seq()'s own comment for exactly what changes. */
    int cv_mode;
    int swing_pct;         /* 50-75, MPC-style 16th swing; 50 = straight.
                           * Whole percent, but the chain_param below is
                           * declared "float" (like Length A/B) so the knob
                           * rides the range-normalised curve instead of
                           * one-step-per-detent -- an int chain_param over
                           * this same 50-75 span felt too twitchy. */
    long swing_pulse_idx;  /* count of 16th pulses fired since Start -- pulse 0
                            * is always exactly on the grid, odd pulses land
                            * late and the following even pulse lands early by
                            * the same amount, so tempo never drifts. Shared
                            * between the internal free-run clock and the
                            * external 24 PPQN follow so swing survives a
                            * hand-off between the two. */

    /* Shared clock -- ported from tb3po's dual-slot clock handling. */
    float bpm;
    int running;
    int follow_transport;
    int clock_pulses;
    double samples_per_step;
    double sample_accum;
    int pulse_sync_active;
    long blocks_since_last_pulse;
    int poll_counter;
    float host_bpm;
    int host_clock_status;
    int clock_stable_count;

    long bar_step_count;  /* 16th-notes advanced since the last bar-reset */

    /* Advanced page -- shared across both sequencers. Per-seq Auto Regen
     * (a_auto_gen/b_auto_gen) lives on acid_seq_t instead -- see its comment
     * there. */
    float jitter;              /* 0.0..1.0: per-tick chance of perturbing WHICH
                               * step plays (not WHEN -- kept clear of swing) */
} acid_inst_t;

static const host_api_v1_t *g_host = NULL;

/* ---------------------------------------------------------------------- */
/* PRNG                                                                    */
/* ---------------------------------------------------------------------- */

static uint32_t rng_next_u32(uint32_t *rng) {
    uint32_t x = *rng;
    if (x == 0) x = 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x;
    return x;
}

static float rng_next_f(uint32_t *rng) {
    return (float)(rng_next_u32(rng) & 0xFFFFFF) / (float)0x1000000;
}

/* ---------------------------------------------------------------------- */
/* Primary generator -- tb3po's density/accent/slide/octave model verbatim */
/* ---------------------------------------------------------------------- */

static void gen_primary(const acid_seq_t *s, uint32_t *rng, int scale_idx,
                         uint8_t *out_steps, uint8_t *out_deg, uint8_t *out_oct) {
    const scale_t *sc = &SCALES[scale_idx];
    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(rng) > s->density) {
            out_steps[i] = STEP_REST;
            continue;
        }
        int on_beat = (i % 4 == 0);
        int degree;
        if (on_beat && rng_next_f(rng) < 0.35f) {
            degree = 0;
        } else {
            degree = (int)(rng_next_f(rng) * (float)sc->len);
            if (degree >= sc->len) degree = sc->len - 1;
        }
        out_deg[i] = (uint8_t)degree;

        int oct = (int)(rng_next_f(rng) * (float)s->octave_range);
        if (oct >= s->octave_range) oct = s->octave_range - 1;
        out_oct[i] = (uint8_t)oct;

        int kind = STEP_NOTE;
        if (rng_next_f(rng) < s->accent) kind = STEP_ACCENT;
        if (rng_next_f(rng) < s->slide) kind = STEP_SLIDE;
        out_steps[i] = (uint8_t)kind;
    }

    /* Slide-into-rest is a no-op -- demote to a plain note. */
    for (int i = 0; i < s->length; i++) {
        int nxt = (i + 1) % s->length;
        if (out_steps[i] == STEP_SLIDE && out_steps[nxt] == STEP_REST) out_steps[i] = STEP_NOTE;
    }

    int any = 0;
    for (int i = 0; i < s->length; i++) if (out_steps[i] != STEP_REST) { any = 1; break; }
    if (!any) { out_steps[0] = STEP_NOTE; out_deg[0] = 0; out_oct[0] = 0; }
}

/* ---------------------------------------------------------------------- */
/* Secondary generator -- Sting.amxd-inspired                              */
/* ---------------------------------------------------------------------- */

static void gen_secondary(const acid_seq_t *s, uint32_t *rng, int scale_idx,
                           uint8_t *out_steps, uint8_t *out_deg, uint8_t *out_oct) {
    const scale_t *sc = &SCALES[scale_idx];

    /* "Classic" (urn 12/16 in the patch): a bag of scale-degree indices,
     * drawn without repeat until exhausted, then reshuffled by refilling. */
    uint8_t bag[12];
    int bag_n = sc->len;
    for (int k = 0; k < bag_n; k++) bag[k] = (uint8_t)k;

    /* "CakeWalk": a bounded random walk drives gate/rest density instead of
     * an independent per-step coin flip. Density knob sets where the walk
     * starts (and roughly where it idles). */
    int walk = (int)(s->density * 8.0f);
    if (walk < 0) walk = 0;
    if (walk > 8) walk = 8;

    /* "VelPyra": mark the first N pyramid slots (N set by the Accent knob)
     * as accented, instead of rolling accent independently per step. */
    int accent_count = (int)(s->accent * 16.0f + 0.5f);
    if (accent_count > 16) accent_count = 16;
    uint8_t accented[MAX_STEPS];
    memset(accented, 0, sizeof(accented));
    for (int k = 0; k < accent_count; k++) {
        int pos = VEL_PYRAMID[k] % s->length;
        accented[pos] = 1;
    }

    for (int i = 0; i < s->length; i++) {
        int step_delta = (int)(rng_next_f(rng) * 3.0f) - 1; /* -1, 0, +1 */
        walk += step_delta;
        if (walk < 0) walk = 0;
        if (walk > 8) walk = 8;

        if (walk < 2) {
            out_steps[i] = STEP_REST;
            continue;
        }

        if (bag_n == 0) {
            bag_n = sc->len;
            for (int k = 0; k < bag_n; k++) bag[k] = (uint8_t)k;
        }
        int idx = (int)(rng_next_f(rng) * (float)bag_n);
        if (idx >= bag_n) idx = bag_n - 1;
        int degree = bag[idx];
        bag[idx] = bag[--bag_n];
        out_deg[i] = (uint8_t)degree;

        int oct = (int)(rng_next_f(rng) * (float)s->octave_range);
        if (oct >= s->octave_range) oct = s->octave_range - 1;
        out_oct[i] = (uint8_t)oct;

        int kind = accented[i] ? STEP_ACCENT : STEP_NOTE;
        if (rng_next_f(rng) < s->slide) kind = STEP_SLIDE;
        out_steps[i] = (uint8_t)kind;
    }

    for (int i = 0; i < s->length; i++) {
        int nxt = (i + 1) % s->length;
        if (out_steps[i] == STEP_SLIDE && out_steps[nxt] == STEP_REST) out_steps[i] = STEP_NOTE;
    }

    int any = 0;
    for (int i = 0; i < s->length; i++) if (out_steps[i] != STEP_REST) { any = 1; break; }
    if (!any) { out_steps[0] = STEP_NOTE; out_deg[0] = 0; out_oct[0] = 0; }
}

/* Algo (1-16) sets the per-step substitution weight of secondary into
 * primary: 1 = 100% primary (tb3po's exact behaviour), 16 = mostly
 * secondary -- directly reproducing the mechanism a comment in Sting.amxd
 * describes ("which notes in the random sequence will be replaced by the
 * 2nd generator and how often"). One evolving PRNG stream is threaded
 * through primary generation, secondary generation and the mix decision,
 * and its final state is persisted so a later Mutate keeps evolving from
 * here rather than repeating itself. */
static void regenerate_pattern(acid_seq_t *s, int scale_idx, uint32_t seed) {
    uint32_t rng = seed ? seed : 1;
    s->seed = seed;

    uint8_t p_steps[MAX_STEPS], p_deg[MAX_STEPS], p_oct[MAX_STEPS];
    gen_primary(s, &rng, scale_idx, p_steps, p_deg, p_oct);

    float weight = (float)(s->algo - 1) / 15.0f;
    if (weight <= 0.0f) {
        memcpy(s->steps, p_steps, MAX_STEPS);
        memcpy(s->degrees, p_deg, MAX_STEPS);
        memcpy(s->octaves, p_oct, MAX_STEPS);
        s->rng = rng;
        return;
    }

    uint8_t sec_steps[MAX_STEPS], sec_deg[MAX_STEPS], sec_oct[MAX_STEPS];
    gen_secondary(s, &rng, scale_idx, sec_steps, sec_deg, sec_oct);

    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(&rng) < weight) {
            s->steps[i] = sec_steps[i]; s->degrees[i] = sec_deg[i]; s->octaves[i] = sec_oct[i];
        } else {
            s->steps[i] = p_steps[i]; s->degrees[i] = p_deg[i]; s->octaves[i] = p_oct[i];
        }
    }
    s->rng = rng;
}

/* Nudge ~25% of steps -- ported from tb3po's mutate_pattern. Algorithm-
 * agnostic: it nudges whatever pattern is currently live, regardless of
 * which Algo blend produced it. */
static void mutate_pattern(acid_seq_t *s, int scale_idx) {
    const scale_t *sc = &SCALES[scale_idx];
    uint32_t rng = s->rng;
    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(&rng) >= 0.25f) continue;
        if (rng_next_f(&rng) < 0.5f) {
            if (rng_next_f(&rng) > s->density) {
                s->steps[i] = STEP_REST;
            } else {
                int kind = STEP_NOTE;
                if (rng_next_f(&rng) < s->accent) kind = STEP_ACCENT;
                if (rng_next_f(&rng) < s->slide) kind = STEP_SLIDE;
                s->steps[i] = (uint8_t)kind;
            }
        } else {
            int degree = (int)(rng_next_f(&rng) * (float)sc->len);
            if (degree >= sc->len) degree = sc->len - 1;
            s->degrees[i] = (uint8_t)degree;
            int oct = (int)(rng_next_f(&rng) * (float)s->octave_range);
            if (oct >= s->octave_range) oct = s->octave_range - 1;
            s->octaves[i] = (uint8_t)oct;
        }
    }
    s->rng = rng;
}

/* ---------------------------------------------------------------------- */
/* Playback                                                                */
/* ---------------------------------------------------------------------- */

/* Offset (Advanced page) is a pure read-side rotation: the stored pattern is
 * never modified, we just index it `offset` steps further along. length >= 2
 * and both operands are non-negative, so the result is always in range. */
static int play_idx(const acid_seq_t *s, int pos) {
    return (pos + s->offset) % s->length;
}

static int note_for_step(const acid_seq_t *s, int scale_idx, int root, int transpose, int step_idx) {
    const scale_t *sc = &SCALES[scale_idx];
    /* Base in the C1 octave, matching tb3po -- keeps the emitted range well
     * clear of Move's pad-LED note range. `transpose` is the live offset from
     * incoming notes (0 = play at the Root knob's key); `s->tune` is this
     * sequencer's own interval on top of that. */
    int base = 24 + root + transpose + s->tune;
    int note = base + sc->degrees[s->degrees[step_idx]] + 12 * s->octaves[step_idx];
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

/* Advance `position` one step per the Direction mode. Pendulum is stateful --
 * it flips pendulum_fwd at each end -- so this takes a mutable pointer rather
 * than staying the pure function it was. Endpoints are turn-around points,
 * not repeated: 0,1,2,..,N-1,N-2,..,1,0,1,.. */
static int next_position(acid_seq_t *s) {
    int n = s->length;
    if (n <= 1) return 0;
    switch (s->dir) {
        case 1: /* Rev */
            return (s->position - 1 + n) % n;
        case 2: /* Pendulum */
            if (s->pendulum_fwd) {
                if (s->position >= n - 1) { s->pendulum_fwd = 0; return n - 2; }
                return s->position + 1;
            }
            if (s->position <= 0) { s->pendulum_fwd = 1; return 1; }
            return s->position - 1;
        default: /* Fwd */
            return (s->position + 1) % n;
    }
}

/* Blend scales each sequencer's OWN velocity by a 0-100%
 * multiplier -- it never substitutes in an absolute target velocity. That
 * keeps each sequence's internal accent/normal ratio (118 vs 72, see
 * emit_step_for_seq) intact; only the relative balance between A and B
 * moves. Full CCW (-63) = A at 100%, B at 0%. Centre (0) = both at 100%.
 * Full CW (+64) = A at 0%, B at 100%. Each side only ever pulls down the
 * OPPOSITE sequence -- the "home" side for a given half stays at 100%
 * throughout that half, ramping linearly from 100% to 0% only as the knob
 * crosses from centre to its far extreme. */
static void compute_blend_velocities(int blend, int *vel_a, int *vel_b) {
    float va, vb;
    if (blend <= 0) {
        float t = (float)(-blend) / 63.0f; /* 0 at centre .. 1 at -63 */
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        va = 127.0f;              /* A stays full on this half */
        vb = 127.0f * (1.0f - t); /* B ramps 100% -> 0% */
    } else {
        float t = (float)blend / 64.0f; /* 0 at centre .. 1 at +64 */
        if (t > 1.0f) t = 1.0f;
        va = 127.0f * (1.0f - t); /* A ramps 100% -> 0% */
        vb = 127.0f;               /* B stays full on this half */
    }
    *vel_a = (int)(va + 0.5f);
    *vel_b = (int)(vb + 0.5f);
    if (*vel_a < 0) *vel_a = 0;
    if (*vel_a > 127) *vel_a = 127;
    if (*vel_b < 0) *vel_b = 0;
    if (*vel_b > 127) *vel_b = 127;
}

static void recompute_step_length(acid_inst_t *t, int sample_rate) {
    double sixteenths_per_sec = (double)t->bpm / 60.0 * 4.0;
    if (sixteenths_per_sec <= 0) sixteenths_per_sec = 8.0;
    t->samples_per_step = (double)sample_rate / sixteenths_per_sec;
}

/* Swing -- MPC-style 16th: swing_pct runs 50 (straight) to 75 (the point
 * past which it stops reading as swing and starts reading as a different
 * subdivision, per the classic MPC ceiling), same convention Move's own
 * Groove control approximates -- 0%/100%/beyond map onto 50%/66.7%/75%
 * here. 50% -> 2/3 gives the textbook 8th-note-triplet feel; 75% pushes
 * the off-16th almost onto the next downbeat.
 *
 * Delayed as a fraction of one *pair* of 16th steps: the odd (off-beat)
 * pulse of each pair arrives late by this fraction of a step, and the
 * following even pulse arrives early by the same amount, so every pair
 * still spans exactly two steps and the average tempo never drifts. */
static double swing_delay_frac(const acid_inst_t *t) {
    if (t->swing_pct <= 50) return 0.0;
    double d = (double)(t->swing_pct - 50) / 50.0; /* 0 .. 0.5 at pct=75 */
    if (d > 0.5) d = 0.5;
    return d;
}

/* Multiplier on samples_per_step for the gap leading INTO the next pulse
 * (internal free-run clock). Pulse 0 (first step after Start) is always
 * exactly on the grid. */
static double swing_gap_mult(const acid_inst_t *t) {
    if (t->swing_pulse_idx <= 0) return 1.0;
    double d = swing_delay_frac(t);
    if (d <= 0.0) return 1.0;
    return (t->swing_pulse_idx & 1) ? (1.0 + d) : (1.0 - d);
}

/* Same idea in units of 24-PPQN clock pulses (6 nominal per 16th step),
 * for the external MIDI-clock-follow path. Rounds to whole pulses since
 * that clock can't subdivide further; a pair (e.g. 8+4 at max swing)
 * still sums to the unswung 12. */
static int swing_pulse_target(const acid_inst_t *t) {
    double mult = swing_gap_mult(t);
    int target = (int)(6.0 * mult + 0.5);
    if (target < 1) target = 1;
    return target;
}

static int kill_seq_note(acid_inst_t *t, int seq_idx, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    (void)t;
    acid_seq_t *s = &t->seq[seq_idx];
    if (s->last_note_on < 0 || max_out < 1) return 0;
    out_msgs[0][0] = 0x80 | (uint8_t)s->out_ch;  /* FORCE-ONLY: was "0x80 | OUT_CH" */
    out_msgs[0][1] = (uint8_t)s->last_note_on;
    out_msgs[0][2] = 0;
    out_lens[0] = 3;
    s->last_note_on = -1;
    return 1;
}

static int kill_all_notes(acid_inst_t *t, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    for (int i = 0; i < NUM_SEQS && count < max_out; i++) {
        count += kill_seq_note(t, i, &out_msgs[count], &out_lens[count], max_out - count);
    }
    return count;
}

/* Emits (or silences) one sequencer's current step. vel_scale (0-127) is
 * this sequencer's Blend-derived level; the step's own accent/normal
 * velocity is scaled by it, so Blend acts as an overall mix level per
 * generator rather than overriding accent dynamics. vel_scale == 0 mutes
 * the sequencer outright for this step (true "only" semantics at the
 * Blend extremes, not just quiet).
 *
 * FORCE-ONLY: CV Mode (t->cv_mode) retargets both of the above for a Force
 * CV track feeding external CV/Gate/Accent/Slide hardware (e.g. a Behringer
 * TD-3-MO) instead of a MIDI synth:
 *   - accent is carried by velocity ALONE, at the widest possible swing (1
 *     for a normal note, 127 for an accented one) so a Force CV row assigned
 *     to Velocity reads a clean, near-full-scale step between "no accent"
 *     and "full accent" -- the 72/118 pair below is tuned for how a
 *     velocity-sensitive synth's own dynamics respond, not for a linear CV
 *     line, and is far too narrow a swing for that use.
 *   - Blend's vel_scale is deliberately NOT applied to the CV-mode velocity:
 *     Blend crossfades two sequencers sharing ONE audio destination, but CV
 *     Mode is for routing each sequencer to its OWN separate CV/Gate
 *     hardware, so scaling the accent-CV level by an unrelated mix control
 *     would corrupt the reading (a mid-Blend setting would report a full
 *     accent as something less than full-scale).
 *   - slide is sent as CC1 (Mod Wheel) instead of CC65 (Portamento), since
 *     Mod Wheel is the CC a Force CV track's own CV-row assignment menu
 *     expects for a non-pitch/velocity/gate row (see docs/CC-MAP.md). The
 *     on/off semantics (127 on, 0 off) are otherwise unchanged. */
static int emit_step_for_seq(acid_inst_t *t, int seq_idx, int prev_pos, int vel_scale,
                              uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_seq_t *s = &t->seq[seq_idx];
    int count = 0;
    int pidx = play_idx(s, s->position);
    uint8_t kind = s->steps[pidx];

    if (kind == STEP_REST || vel_scale <= 0) {
        if (count < max_out) count += kill_seq_note(t, seq_idx, &out_msgs[count], &out_lens[count], max_out - count);
        s->gate_samples_remaining = 0;
        return count;
    }

    int note = note_for_step(s, t->scale, t->root, t->live_transpose, pidx);
    int is_accent = (kind == STEP_ACCENT);
    int vel;
    if (t->cv_mode) {
        vel = is_accent ? 127 : 1;
    } else {
        int base_vel = is_accent ? 118 : 72;
        vel = (base_vel * vel_scale) / 127;
        if (vel < 1) vel = 1;
        if (vel > 127) vel = 127;
    }

    uint8_t slide_cc = t->cv_mode ? 1 : 65;   /* FORCE-ONLY: Mod Wheel in CV Mode */

    int was_slide = (prev_pos >= 0 && s->steps[play_idx(s, prev_pos)] == STEP_SLIDE);

    /* FORCE-ONLY: every 0x80/0x90/0xB0 status byte below uses s->out_ch
     * (a_channel/b_channel) instead of upstream's fixed OUT_CH. */
    if (was_slide) {
        if (!s->portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | (uint8_t)s->out_ch; out_msgs[count][1] = slide_cc; out_msgs[count][2] = 127;
            out_lens[count] = 3; count++;
            s->portamento_on = 1;
        }
        if (count < max_out) {
            out_msgs[count][0] = 0x90 | (uint8_t)s->out_ch; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
            out_lens[count] = 3; count++;
        }
        if (s->last_note_on >= 0 && s->last_note_on != note && count < max_out) {
            out_msgs[count][0] = 0x80 | (uint8_t)s->out_ch; out_msgs[count][1] = (uint8_t)s->last_note_on; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
        }
    } else {
        if (s->portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | (uint8_t)s->out_ch; out_msgs[count][1] = slide_cc; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->portamento_on = 0;
        }
        if (s->last_note_on >= 0 && count < max_out) {
            count += kill_seq_note(t, seq_idx, &out_msgs[count], &out_lens[count], max_out - count);
        }
        if (count < max_out) {
            out_msgs[count][0] = 0x90 | (uint8_t)s->out_ch; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
            out_lens[count] = 3; count++;
        }
    }
    s->last_note_on = note;
    s->gate_samples_remaining = (long)(t->samples_per_step * s->gate);
    return count;
}

/* FORCE-ONLY: advances one sequencer's "Export as MIDI Clip" dump player by
 * one call's worth of elapsed samples, firing whichever dump step is due on
 * DUMP_STEP_MS's own fixed cadence. Mirrors emit_step_for_seq()'s note/slide
 * logic but reads/writes the seq's dump_* fields instead of the live
 * position/last_note_on/portamento_on -- so a dump in progress can never
 * disturb (or be disturbed by) live playback of this sequencer or the
 * other one. Deliberately always uses the plain MIDI convention (72/118
 * velocity, CC65 slide) regardless of t->cv_mode, and ignores Blend
 * entirely (this sequencer's own pattern at full strength) -- an exported
 * clip is meant to be a normal, reusable MIDI clip, not tied to whatever
 * live routing/mix mode happened to be active when it was captured.
 *
 * Runs `s->length` steps, then one extra terminal tick that releases
 * whatever note/portamento is still held and sends a distinctive
 * completion marker (CC3=127 on this seq's own channel) before clearing
 * dump_active -- so a listener knows the dump is done without having to
 * guess from elapsed time. */
static int process_dump_for_seq(acid_inst_t *t, int seq_idx, int frames, int sample_rate,
                                 uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_seq_t *s = &t->seq[seq_idx];
    if (!s->dump_active) return 0;

    s->dump_sample_accum += frames;
    long step_samples = (long)((double)sample_rate * DUMP_STEP_MS / 1000.0);
    if (s->dump_sample_accum < step_samples) return 0;
    s->dump_sample_accum -= step_samples;

    int count = 0;

    if (s->dump_pos >= s->length) {
        if (s->dump_portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | DUMP_CHANNEL; out_msgs[count][1] = 65; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->dump_portamento_on = 0;
        }
        if (s->dump_last_note >= 0 && count < max_out) {
            out_msgs[count][0] = 0x80 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)s->dump_last_note; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->dump_last_note = -1;
        }
        if (count < max_out) {
            out_msgs[count][0] = 0xB0 | DUMP_CHANNEL; out_msgs[count][1] = 3; out_msgs[count][2] = 127;
            out_lens[count] = 3; count++;
        }
        s->dump_active = 0;
        return count;
    }

    int pidx = play_idx(s, s->dump_pos);
    uint8_t kind = s->steps[pidx];
    int prev_pos = s->dump_pos - 1;   /* -1 on the first step -- no prior step to slide from */
    int was_slide = (prev_pos >= 0 && s->steps[play_idx(s, prev_pos)] == STEP_SLIDE);

    if (kind == STEP_REST) {
        if (s->dump_portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | DUMP_CHANNEL; out_msgs[count][1] = 65; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->dump_portamento_on = 0;
        }
        if (s->dump_last_note >= 0 && count < max_out) {
            out_msgs[count][0] = 0x80 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)s->dump_last_note; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->dump_last_note = -1;
        }
    } else {
        int note = note_for_step(s, t->scale, t->root, t->live_transpose, pidx);
        int is_accent = (kind == STEP_ACCENT);
        int vel = is_accent ? 118 : 72;

        if (was_slide) {
            if (!s->dump_portamento_on && count < max_out) {
                out_msgs[count][0] = 0xB0 | DUMP_CHANNEL; out_msgs[count][1] = 65; out_msgs[count][2] = 127;
                out_lens[count] = 3; count++;
                s->dump_portamento_on = 1;
            }
            if (count < max_out) {
                out_msgs[count][0] = 0x90 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
                out_lens[count] = 3; count++;
            }
            if (s->dump_last_note >= 0 && s->dump_last_note != note && count < max_out) {
                out_msgs[count][0] = 0x80 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)s->dump_last_note; out_msgs[count][2] = 0;
                out_lens[count] = 3; count++;
            }
        } else {
            if (s->dump_portamento_on && count < max_out) {
                out_msgs[count][0] = 0xB0 | DUMP_CHANNEL; out_msgs[count][1] = 65; out_msgs[count][2] = 0;
                out_lens[count] = 3; count++;
                s->dump_portamento_on = 0;
            }
            if (s->dump_last_note >= 0 && count < max_out) {
                out_msgs[count][0] = 0x80 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)s->dump_last_note; out_msgs[count][2] = 0;
                out_lens[count] = 3; count++;
            }
            if (count < max_out) {
                out_msgs[count][0] = 0x90 | DUMP_CHANNEL; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
                out_lens[count] = 3; count++;
            }
        }
        s->dump_last_note = note;
    }

    s->dump_pos++;
    return count;
}

/* Advances both sequencers by one 16th-note tick, applies the Reset Both
 * bar-boundary snap when armed, and emits through the Blend crossfade.
 * Each sequencer wraps independently at its own Length every tick -- that
 * independent wrap is what makes differently-lengthed A/B patterns a real
 * polymeter when Reset Both is Off, rather than something that needs
 * explicit polymeter support. */
static int advance_all(acid_inst_t *t, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;

    int forced_reset = 0;
    if (t->reset_bars_idx != 4) {
        t->bar_step_count++;
        if (t->bar_step_count >= BAR_STEPS[t->reset_bars_idx]) {
            t->bar_step_count = 0;
            forced_reset = 1;
        }
    } else {
        t->bar_step_count = 0;
    }

    /* Auto Regen (Advanced) -- FORCE-ONLY: independent per-seq bar counter
     * (see acid_seq_t's comment), each running against Reset Both's shared
     * counter but not against each other. On its own boundary, re-roll just
     * that sequencer from a fresh seed (the same call the "generate" param
     * makes). Done before the position snap / emit below, so the freshly
     * generated pattern is what plays this tick. If a seq's Auto Regen and
     * Reset Both are set to the same interval they fire on the same tick:
     * regenerate first, then snap to step 0 -- intended. */
    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        if (s->auto_gen_idx != 0) {
            s->auto_gen_step_count++;
            if (s->auto_gen_step_count >= AUTO_GEN_BAR_STEPS[s->auto_gen_idx - 1]) {
                s->auto_gen_step_count = 0;
                regenerate_pattern(s, t->scale, rng_next_u32(&s->rng));
                if (s->position >= s->length) s->position = s->length - 1;
            }
        } else {
            s->auto_gen_step_count = 0;
        }
    }

    int vel_a, vel_b;
    compute_blend_velocities(t->blend, &vel_a, &vel_b);

    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        int prev = s->position;
        if (forced_reset) {
            s->position = 0;
            s->pendulum_fwd = 1;  /* Reset Both is authoritative -- pendulum resumes forward */
        } else {
            int np = next_position(s);
            /* Jitter (Advanced) -- perturb WHICH step plays, never WHEN, so it
             * stays independent of swing_gap_mult()/swing_delay_frac(). Rolls
             * off the same per-seq PRNG stream Mutate draws from, so it stays
             * deterministic relative to the seed. Three outcomes, split evenly
             * for now: skip an extra step, repeat the step just left, or jump
             * somewhere random. The exact split is an ear-check call -- flagged
             * like the Algo blend curve and the VelPyra table, not settled. */
            if (t->jitter > 0.0f && s->length > 1 &&
                rng_next_f(&s->rng) < t->jitter) {
                switch (rng_next_u32(&s->rng) % 3u) {
                    case 0: np = (np + 1) % s->length; break;
                    case 1: np = prev; break;
                    default: {
                        int r = (int)(rng_next_f(&s->rng) * (float)s->length);
                        if (r >= s->length) r = s->length - 1;
                        np = r;
                    }
                }
            }
            s->position = np;
        }
        int vel_scale = (i == SEQ_A) ? vel_a : vel_b;
        if (count < max_out) {
            count += emit_step_for_seq(t, i, prev, vel_scale, &out_msgs[count], &out_lens[count], max_out - count);
        }
    }
    return count;
}

/* ---------------------------------------------------------------------- */
/* Plugin API surface                                                     */
/* ---------------------------------------------------------------------- */

static void *acid_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    acid_inst_t *t = (acid_inst_t *)calloc(1, sizeof(acid_inst_t));
    if (!t) return NULL;

    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        s->rng = 0xBEEFu + (uint32_t)i;
        s->seed = s->rng;
        s->length = 16;
        s->density = 0.7f;
        s->accent = 0.4f;
        s->slide = 0.25f;
        s->octave_range = 2;
        s->gate = 0.5f;
        s->algo = 1;
        s->tune = 0;
        s->last_note_on = -1;
        s->dump_last_note = -1;   /* FORCE-ONLY: -1 = none, same sentinel as last_note_on */
        s->pendulum_fwd = 1; /* offset/dir/out_ch default to 0 (Fwd, no rotation, channel 1) via calloc */
    }
    t->root = 9; /* A, matches tb3po's default */
    t->live_transpose = 0;
    t->scale = 0;
    t->blend = -63; /* Seq A alone until Blend is dialled up */
    t->reset_bars_idx = 4; /* Off */
    t->swing_pct = 50; /* straight */
    t->swing_pulse_idx = 0;
    t->bpm = 120.0f;
    t->running = 0;
    t->follow_transport = 1;

    if (g_host && g_host->get_clock_status) {
        if (g_host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING) t->running = 1;
    }

    recompute_step_length(t, MOVE_SAMPLE_RATE);

    for (int i = 0; i < NUM_SEQS; i++) {
        regenerate_pattern(&t->seq[i], t->scale, t->seq[i].seed);
        t->seq[i].position = t->seq[i].length - 1; /* first advance lands on 0 */
    }

    return t;
}

static void acid_destroy_instance(void *instance) {
    /* No output channel available here (set_param/destroy_instance carry no
     * out_msgs) to send note-offs -- the chain host is responsible for
     * silencing voices on unload, same as every other midi_fx module. */
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t) return;
    free(t);
}

static int acid_process_midi(void *instance, const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0];
    uint8_t type = status & 0xF0;

    if (status == 0xF8) { /* MIDI clock tick, 24 PPQN */
        t->pulse_sync_active = 1;
        t->blocks_since_last_pulse = 0;
        if (t->running) {
            t->clock_pulses++;
            if (t->clock_pulses >= swing_pulse_target(t)) {
                t->clock_pulses = 0;
                int fired = advance_all(t, out_msgs, out_lens, max_out);
                t->swing_pulse_idx++;
                return fired;
            }
        }
        return 0;
    }
    if (status == 0xFA) { /* Start */
        if (t->follow_transport) {
            t->running = 1;
            for (int i = 0; i < NUM_SEQS; i++) {
                t->seq[i].position = t->seq[i].length - 1;
                t->seq[i].pendulum_fwd = 1;
                t->seq[i].auto_gen_step_count = 0;
            }
            t->clock_pulses = 5; /* first 0xF8 bumps to 0 -> fires step 0 on the downbeat */
            t->sample_accum = 0;
            t->bar_step_count = 0;
            t->swing_pulse_idx = 0;
        }
        return 0;
    }
    if (status == 0xFB) { /* Continue */
        if (t->follow_transport) t->running = 1;
        return 0;
    }
    if (status == 0xFC) { /* Stop */
        if (t->follow_transport) {
            t->running = 0;
            return kill_all_notes(t, out_msgs, out_lens, max_out);
        }
        return 0;
    }

    /* Hybrid trigger model: an incoming note transposes both sequencers live
     * (relative to C4), leaving the Root knob/field untouched -- so a clip on
     * the track, a stray pad, or an echo of our own output can't drag Root
     * around. Sequencing is not gated by note input. Note-on/off are both
     * swallowed (Acid generates its own stream, it does not pass notes through).
     * Unchanged from upstream -- still applies to BOTH sequencers regardless
     * of their (possibly different) output channels; transpose is a single
     * shared control, not per-channel. */
    if (type == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        int tr = (int)in_msg[1] - ACID_ANCHOR_NOTE;
        if (tr < -ACID_MAX_TRANSPOSE) tr = -ACID_MAX_TRANSPOSE;
        if (tr >  ACID_MAX_TRANSPOSE) tr =  ACID_MAX_TRANSPOSE;
        t->live_transpose = tr;
        return 0;
    }
    if (type == 0x80 || (type == 0x90 && in_len >= 3 && in_msg[2] == 0)) {
        return 0;
    }

    /* Pass everything else (CC, pitch bend, program change...) through --
     * same convention as the built-in arp. FORCE-ONLY: upstream echoes
     * in_msg[0] (the control channel) completely unchanged; on Force that
     * channel usually isn't where a synth is listening once A/B have their
     * own output channels, so channel-voice messages are retargeted to Seq
     * A's channel. Note/CC65 emission from emit_step_for_seq() already
     * carries the correct per-seq channel and is untouched by this. */
    uint8_t st0 = in_msg[0] & 0xF0;
    out_msgs[0][0] = (st0 < 0xF0) ? (uint8_t)(st0 | (uint8_t)t->seq[SEQ_A].out_ch) : in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int acid_tick(void *instance, int frames, int sample_rate,
                     uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t) return 0;
    int count = 0;

    if (t->pulse_sync_active) {
        t->blocks_since_last_pulse++;
        if (t->blocks_since_last_pulse > 200) { /* ~580ms of silence -> fall back to internal timer */
            t->pulse_sync_active = 0;
            t->clock_pulses = 0;
        }
        t->sample_accum = 0;
    }

    if ((++t->poll_counter & 0x1F) == 0 && g_host && g_host->get_bpm) {
        float hb = g_host->get_bpm();
        if (hb > 20.0f && hb < 400.0f && hb != t->host_bpm) {
            t->host_bpm = hb;
            t->bpm = hb;
            recompute_step_length(t, sample_rate);
        }
    }

    if (t->follow_transport && g_host && g_host->get_clock_status && ((t->poll_counter & 0x07) == 0)) {
        int cs = g_host->get_clock_status();
        if (cs == t->host_clock_status) {
            if (t->clock_stable_count < 32) t->clock_stable_count++;
        } else {
            t->host_clock_status = cs;
            t->clock_stable_count = 1;
        }
        if (t->clock_stable_count >= 8) {
            if (cs == MOVE_CLOCK_STATUS_RUNNING && !t->running) {
                t->running = 1;
                for (int i = 0; i < NUM_SEQS; i++) {
                    t->seq[i].position = t->seq[i].length - 1;
                    t->seq[i].pendulum_fwd = 1;
                    t->seq[i].auto_gen_step_count = 0;
                }
                t->clock_pulses = 5;
                t->sample_accum = 0;
                t->bar_step_count = 0;
                t->swing_pulse_idx = 0;
            } else if (cs == MOVE_CLOCK_STATUS_STOPPED && t->running) {
                t->running = 0;
                if (count < max_out) count += kill_all_notes(t, &out_msgs[count], &out_lens[count], max_out - count);
            }
        }
    }

    /* FORCE-ONLY: Export dump players run unconditionally, before the
     * !t->running early-return below -- a dump must complete whether or not
     * the live transport is running, and must never touch (or be touched
     * by) live playback's own advance_all()/position state. */
    for (int i = 0; i < NUM_SEQS && count < max_out; i++) {
        count += process_dump_for_seq(t, i, frames, sample_rate, &out_msgs[count], &out_lens[count], max_out - count);
    }

    if (!t->running) return count;

    if (!t->pulse_sync_active) {
        t->sample_accum += (double)frames;
        while (count < max_out - 4) {
            double gap = t->samples_per_step * swing_gap_mult(t);
            if (t->sample_accum < gap) break;
            t->sample_accum -= gap;
            count += advance_all(t, &out_msgs[count], &out_lens[count], max_out - count);
            t->swing_pulse_idx++;
        }
    }

    /* Gate-off accounting, per sequencer -- unless the next-held step is a
     * slide (slide holds the note until the following note replaces it). */
    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        if (s->gate_samples_remaining > 0) {
            s->gate_samples_remaining -= frames;
            if (s->gate_samples_remaining <= 0) {
                s->gate_samples_remaining = 0;
                if (s->steps[play_idx(s, s->position)] != STEP_SLIDE && count < max_out) {
                    count += kill_seq_note(t, i, &out_msgs[count], &out_lens[count], max_out - count);
                }
            }
        }
    }
    return count;
}

static int parse_int(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    return (int)strtol(s, NULL, 10);
}
static float parse_float(const char *s, float fallback) {
    if (!s || !*s) return fallback;
    return (float)strtod(s, NULL);
}

static void acid_set_param(void *instance, const char *key, const char *val) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key) return;

    int seq_idx = -1;
    const char *k = key;
    if (key[0] == 'a' && key[1] == '_') { seq_idx = SEQ_A; k = key + 2; }
    else if (key[0] == 'b' && key[1] == '_') { seq_idx = SEQ_B; k = key + 2; }

    if (seq_idx >= 0) {
        acid_seq_t *s = &t->seq[seq_idx];
        if (strcmp(k, "generate") == 0) {
            uint32_t seed = rng_next_u32(&s->rng);
            regenerate_pattern(s, t->scale, seed);
            if (s->position >= s->length) s->position = s->length - 1;
        } else if (strcmp(k, "mutate") == 0) {
            mutate_pattern(s, t->scale);
        } else if (strcmp(k, "dump") == 0) {
            /* FORCE-ONLY: (re)start this seq's Export dump player -- see
             * process_dump_for_seq(). Always restarts from step 0, even if
             * a previous dump was still in progress. */
            s->dump_active = 1;
            s->dump_pos = 0;
            s->dump_sample_accum = 0;
            s->dump_last_note = -1;
            s->dump_portamento_on = 0;
        } else if (strcmp(k, "density") == 0) {
            float v = parse_float(val, 0.7f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->density = v;
        } else if (strcmp(k, "accent") == 0) {
            float v = parse_float(val, 0.4f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->accent = v;
        } else if (strcmp(k, "slide") == 0) {
            float v = parse_float(val, 0.25f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->slide = v;
        } else if (strcmp(k, "octaves") == 0) {
            int v = parse_int(val, 2);
            if (v < 1) v = 1;
            if (v > 3) v = 3;
            s->octave_range = v;
        } else if (strcmp(k, "length") == 0) {
            /* Declared as a float chain_param (min 2, max 32) so the knob rides
             * the range-normalised curve instead of one-step-per-detent -- the
             * wire value arrives like "16.000", so round rather than truncate. */
            int v = (int)(parse_float(val, 16.0f) + 0.5f);
            if (v < MIN_LENGTH) v = MIN_LENGTH;
            if (v > MAX_STEPS) v = MAX_STEPS;
            s->length = v;
            if (s->position >= v) s->position = v - 1;
            if (s->offset >= v) s->offset = v - 1;  /* keep Offset < the new length */
        } else if (strcmp(k, "gate") == 0) {
            float v = parse_float(val, 0.5f);
            if (v < 0.05f) v = 0.05f;
            if (v > 1.0f) v = 1.0f;
            s->gate = v;
        } else if (strcmp(k, "algo") == 0) {
            int v = parse_int(val, 1);
            if (v < 1) v = 1;
            if (v > 16) v = 16;
            s->algo = v;
        } else if (strcmp(k, "tune") == 0) {
            int v = parse_int(val, 0);
            if (v < -ACID_MAX_TUNE) v = -ACID_MAX_TUNE;
            if (v >  ACID_MAX_TUNE) v =  ACID_MAX_TUNE;
            s->tune = v;
        } else if (strcmp(k, "offset") == 0) {
            /* Declared as a float chain_param (like Length A/B and Swing) so
             * the knob rides the range-normalised curve instead of stepping
             * one-per-detent across the 0..31 span -- the wire value arrives
             * like "7.000", so round rather than truncate. */
            int v = (int)(parse_float(val, 0.0f) + 0.5f);
            if (v < 0) v = 0;
            if (v >= s->length) v = s->length - 1;
            s->offset = v;
        } else if (strcmp(k, "dir") == 0) {
            int v = parse_int(val, 0);
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            if (v == 2 && s->dir != 2) s->pendulum_fwd = 1; /* enter Pendulum travelling forward */
            s->dir = v;
        } else if (strcmp(k, "channel") == 0) {
            /* FORCE-ONLY: no Move equivalent -- see the file header. 1-16 on
             * the wire (matches control_channel/output_channel convention
             * elsewhere), stored 0-based like every other channel byte here. */
            int v = parse_int(val, 1);
            if (v < 1) v = 1;
            if (v > 16) v = 16;
            s->out_ch = v - 1;
        } else if (strcmp(k, "auto_gen") == 0) {
            /* FORCE-ONLY: per-seq Auto Regen (a_auto_gen/b_auto_gen) -- see
             * acid_seq_t's comment. */
            int v = parse_int(val, 0);
            if (v < 0) v = 0;
            if (v > 6) v = 6;
            s->auto_gen_idx = v;
            s->auto_gen_step_count = 0;
        }
        return;
    }

    if (strcmp(key, "root") == 0) {
        int v = parse_int(val, 9) % 12; if (v < 0) v += 12; t->root = v;
    } else if (strcmp(key, "scale") == 0) {
        int v = parse_int(val, 0); if (v < 0 || v >= NUM_SCALES) v = 0; t->scale = v;
    } else if (strcmp(key, "blend") == 0) {
        int v = parse_int(val, 0);
        if (v < -63) v = -63;
        if (v > 64) v = 64;
        t->blend = v;
    } else if (strcmp(key, "reset_bars") == 0) {
        int v = parse_int(val, 4); if (v < 0 || v > 4) v = 4;
        t->reset_bars_idx = v;
        t->bar_step_count = 0;
    } else if (strcmp(key, "swing") == 0) {
        /* Declared as a float chain_param for the range-normalised knob
         * curve (see the struct comment) -- the wire value arrives like
         * "62.000", so round rather than truncate, same as Length A/B. */
        int v = (int)(parse_float(val, 50.0f) + 0.5f);
        if (v < 50) v = 50;
        if (v > 75) v = 75;
        t->swing_pct = v;
    } else if (strcmp(key, "jitter") == 0) {
        float v = parse_float(val, 0.0f);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        t->jitter = v;
    } else if (strcmp(key, "cv_mode") == 0) {
        /* FORCE-ONLY: see emit_step_for_seq()'s own comment for what this
         * changes (accent-via-velocity swing, slide CC target). */
        int v = parse_int(val, 0);
        t->cv_mode = v ? 1 : 0;
    }
}

static int acid_get_param(void *instance, const char *key, char *buf, int buf_len) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key || !buf || buf_len < 2) return -1;

    int seq_idx = -1;
    const char *k = key;
    if (key[0] == 'a' && key[1] == '_') { seq_idx = SEQ_A; k = key + 2; }
    else if (key[0] == 'b' && key[1] == '_') { seq_idx = SEQ_B; k = key + 2; }

    int n = -1;

    if (seq_idx >= 0) {
        acid_seq_t *s = &t->seq[seq_idx];
        if (strcmp(k, "density") == 0) n = snprintf(buf, buf_len, "%.3f", s->density);
        else if (strcmp(k, "accent") == 0) n = snprintf(buf, buf_len, "%.3f", s->accent);
        else if (strcmp(k, "slide") == 0) n = snprintf(buf, buf_len, "%.3f", s->slide);
        else if (strcmp(k, "octaves") == 0) n = snprintf(buf, buf_len, "%d", s->octave_range);
        else if (strcmp(k, "length") == 0) n = snprintf(buf, buf_len, "%d", s->length);
        else if (strcmp(k, "gate") == 0) n = snprintf(buf, buf_len, "%.3f", s->gate);
        else if (strcmp(k, "algo") == 0) n = snprintf(buf, buf_len, "%d", s->algo);
        else if (strcmp(k, "tune") == 0) n = snprintf(buf, buf_len, "%d", s->tune);
        else if (strcmp(k, "offset") == 0) n = snprintf(buf, buf_len, "%d", s->offset);
        else if (strcmp(k, "dir") == 0) n = snprintf(buf, buf_len, "%d", s->dir);
        else if (strcmp(k, "channel") == 0) n = snprintf(buf, buf_len, "%d", s->out_ch + 1);  /* FORCE-ONLY */
        else if (strcmp(k, "auto_gen") == 0) n = snprintf(buf, buf_len, "%d", s->auto_gen_idx);  /* FORCE-ONLY */
        else if (strcmp(k, "generate") == 0 || strcmp(k, "mutate") == 0 || strcmp(k, "dump") == 0) n = snprintf(buf, buf_len, "off");
        else return -1;
        if (n < 0) return -1;
        if (n >= buf_len) n = buf_len - 1;
        return n;
    }

    if (strcmp(key, "root") == 0) n = snprintf(buf, buf_len, "%d", t->root);
    else if (strcmp(key, "scale") == 0) n = snprintf(buf, buf_len, "%d", t->scale);
    else if (strcmp(key, "blend") == 0) n = snprintf(buf, buf_len, "%d", t->blend);
    else if (strcmp(key, "reset_bars") == 0) n = snprintf(buf, buf_len, "%d", t->reset_bars_idx);
    else if (strcmp(key, "swing") == 0) n = snprintf(buf, buf_len, "%d", t->swing_pct);
    else if (strcmp(key, "jitter") == 0) n = snprintf(buf, buf_len, "%.3f", t->jitter);
    else if (strcmp(key, "cv_mode") == 0) n = snprintf(buf, buf_len, "%d", t->cv_mode);  /* FORCE-ONLY */
    else if (strcmp(key, "chain_params") == 0) {
        /* Not actually consulted for midi_fx loading -- chain_midi.c reads
         * chain_params straight out of module.json on disk (parse_chain_params),
         * falling back to it precisely because our ui_hierarchy params carry
         * only key/short_name, no inline type info. Kept here anyway for
         * parity with the rest of the ecosystem (arp.c does the same) and
         * any other caller that does ask the loaded plugin directly. Must be
         * kept in sync with module.json's chain_params by hand.
         *
         * FORCE-ONLY note: a_channel/b_channel deliberately are NOT added to
         * this string -- it mirrors upstream's own module.json verbatim
         * (Move has no such params), and force-acid's host_shim.cpp never
         * calls get_param("chain_params") anyway. See host_shim.cpp's own
         * PARAMS[] table for the Force-side CC map instead. */
        static const char params[] =
            "["
            "{\"key\":\"a_generate\",\"name\":\"Generate A\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_mutate\",\"name\":\"Mutate A\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_density\",\"name\":\"Density A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7,\"unit\":\"%\"},"
            "{\"key\":\"a_accent\",\"name\":\"Accent A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4,\"unit\":\"%\"},"
            "{\"key\":\"a_slide\",\"name\":\"Slide A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25,\"unit\":\"%\"},"
            "{\"key\":\"a_octaves\",\"name\":\"Octaves A\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"a_length\",\"name\":\"Length A\",\"type\":\"float\",\"min\":2,\"max\":32,\"step\":1,\"default\":16,\"display_format\":\".0f\"},"
            "{\"key\":\"a_gate\",\"name\":\"Gate A\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5,\"unit\":\"%\"},"
            "{\"key\":\"b_generate\",\"name\":\"Generate B\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_mutate\",\"name\":\"Mutate B\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_density\",\"name\":\"Density B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7,\"unit\":\"%\"},"
            "{\"key\":\"b_accent\",\"name\":\"Accent B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4,\"unit\":\"%\"},"
            "{\"key\":\"b_slide\",\"name\":\"Slide B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25,\"unit\":\"%\"},"
            "{\"key\":\"b_octaves\",\"name\":\"Octaves B\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"b_length\",\"name\":\"Length B\",\"type\":\"float\",\"min\":2,\"max\":32,\"step\":1,\"default\":16,\"display_format\":\".0f\"},"
            "{\"key\":\"b_gate\",\"name\":\"Gate B\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5,\"unit\":\"%\"},"
            "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\",\"options\":[\"Minor\",\"Phrygian\",\"HarmMinor\",\"MinPent\",\"Dorian\",\"Major\",\"PhrygDom\",\"Locrian\",\"WholeTone\",\"HungMinor\",\"MinBlues\",\"Chromatic\"],\"default\":0},"
            "{\"key\":\"root\",\"name\":\"Root\",\"type\":\"enum\",\"options\":[\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"],\"default\":9},"
            "{\"key\":\"b_tune\",\"name\":\"Tune B\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1,\"default\":0},"
            "{\"key\":\"blend\",\"name\":\"Blend\",\"type\":\"int\",\"min\":-63,\"max\":64,\"step\":1,\"default\":-63},"
            "{\"key\":\"a_algo\",\"name\":\"Algo A\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"b_algo\",\"name\":\"Algo B\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"reset_bars\",\"name\":\"Reset Both\",\"type\":\"enum\",\"options\":[\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\",\"Off\"],\"default\":4},"
            "{\"key\":\"swing\",\"name\":\"Swing\",\"type\":\"float\",\"min\":50,\"max\":75,\"step\":1,\"default\":50,\"display_format\":\".0f\"},"
            "{\"key\":\"a_offset\",\"name\":\"Offset A\",\"type\":\"float\",\"min\":0,\"max\":31,\"step\":1,\"default\":0,\"display_format\":\".0f\"},"
            "{\"key\":\"b_offset\",\"name\":\"Offset B\",\"type\":\"float\",\"min\":0,\"max\":31,\"step\":1,\"default\":0,\"display_format\":\".0f\"},"
            "{\"key\":\"a_dir\",\"name\":\"Direction A\",\"type\":\"enum\",\"options\":[\"Fwd\",\"Rev\",\"Pendulum\"],\"default\":0},"
            "{\"key\":\"b_dir\",\"name\":\"Direction B\",\"type\":\"enum\",\"options\":[\"Fwd\",\"Rev\",\"Pendulum\"],\"default\":0},"
            "{\"key\":\"jitter\",\"name\":\"Jitter\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.0,\"unit\":\"%\"}"
            /* a_auto_gen/b_auto_gen/cv_mode/a_dump/b_dump deliberately
             * omitted -- all FORCE-ONLY, same convention as a_channel/
             * b_channel above (no Move equivalent). */
            "]";
        n = snprintf(buf, buf_len, "%s", params);
    }
    else return -1;

    if (n < 0) return -1;
    if (n >= buf_len) n = buf_len - 1;
    return n;
}

static midi_fx_api_v1_t acid_api_v1 = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = acid_create_instance,
    .destroy_instance = acid_destroy_instance,
    .process_midi     = acid_process_midi,
    .tick             = acid_tick,
    .set_param        = acid_set_param,
    .get_param        = acid_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &acid_api_v1;
}
