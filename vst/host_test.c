/* Offline x86 host test for acid_vst's VST2 wrapper (no ALSA seq output needed to
 * exercise the wrapper logic -- alsa_open()/alsa_send() no-op gracefully when
 * snd_seq_open() fails, e.g. no /dev/snd here). Build/run under ASan (see
 * docs/PORTING.md "Offline test first"): two instances, param round-trip incl.
 * an enum, a transport-driven tick that should produce audible generator
 * activity (checked indirectly via a param readback), and chunk round-trip. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "params.h"
#include "acid_core.h"

typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void *proc;
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t np, npar, ni, no, flags;
    intptr_t r1, r2;
    int32_t a, b, c;
    float io;
    void *obj, *user;
    int32_t uid, ver;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void *pdr;
    char f[56];
};
typedef struct { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                 int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags; } TI;
enum { kPlaying = 1 << 1, kPpq = 1 << 9, kTempo = 1 << 10 };

extern AEffect *VSTPluginMain(cb);
extern long acid_dbg_pulses(AEffect *);
extern float acid_dbg_bpm(void);
extern int acid_dbg_pos(AEffect *);
extern void acid_dbg_control(AEffect *, int status, int d1, int d2);

static int pidx(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    printf("FAIL no param %s\n", key);
    exit(1);
}

/* ---- engine-level run: one sequencer, N song steps, every message with the step it came in ---- */
typedef struct { int step; uint8_t m[3]; } ev_t;
static float t_bpm(void) { return 120.0f; }
static int t_clock(void) { return MOVE_CLOCK_STATUS_RUNNING; }
static const host_api_v1_t T_HOST = { t_bpm, t_clock };
static int run_core(midi_fx_api_v1_t *api, const char *const *kv, int steps, ev_t *evs, int max) {
    void *inst = api->create_instance(".", NULL);
    for (int i = 0; kv[i]; i += 2) api->set_param(inst, kv[i], kv[i + 1]);
    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
    int olen[MIDI_FX_MAX_OUT_MSGS], n = 0, c;
    uint8_t st = 0xFA, arm[2] = {0xF9, 1};
    api->process_midi(inst, &st, 1, out, olen, MIDI_FX_MAX_OUT_MSGS);
    api->process_midi(inst, arm, 2, out, olen, MIDI_FX_MAX_OUT_MSGS);
    for (int k = 0; k < steps; k++) {
        char kb[16]; snprintf(kb, sizeof kb, "%d", k);
        api->set_param(inst, "song_step", kb);
        st = 0xF9;
        c = api->process_midi(inst, &st, 1, out, olen, MIDI_FX_MAX_OUT_MSGS);
        for (int i = 0; i < c && n < max; i++) { evs[n].step = k; memcpy(evs[n].m, out[i], 3); n++; }
        for (int b = 0; b < 43; b++) {   /* one 16th at 120 BPM = 5512 frames, in 128-frame blocks */
            c = api->tick(inst, 128, MOVE_SAMPLE_RATE, out, olen, MIDI_FX_MAX_OUT_MSGS);
            for (int i = 0; i < c && n < max; i++) { evs[n].step = k; memcpy(evs[n].m, out[i], 3); n++; }
        }
    }
    api->destroy_instance(inst);
    return n;
}

static TI g_ti;
static int automated[NPARAMS];
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)v; (void)p; (void)o;
    if (op == 0 && idx >= 0 && idx < NPARAMS) automated[idx]++;   /* audioMasterAutomate */
    if (op == 7) return (intptr_t)&g_ti;   /* audioMasterGetTime */
    return 0;
}

int main(void) {
    AEffect *a = VSTPluginMain(host), *b = VSTPluginMain(host);
    printf("magic=%x params=%d flags=%x uid=%x twoInstances=%d\n", a->magic, a->npar, a->flags, a->uid, a != b);

    char name[64], disp[64];
    for (int i = 0; i < a->npar; i++) {
        a->d(a, 8, i, 0, name, 0);
        a->d(a, 7, i, 0, disp, 0);
        printf("  p%-2d %-10s = %-8s (norm %.3f)\n", i, name, disp, a->getP(a, i));
    }

    /* enum round-trip: a_dir (index 10) -> its last option */
    a->setP(a, 10, 1.0f);
    a->d(a, 7, 10, 0, disp, 0);
    printf("%s -> 1.0 => %s (expect %s)\n", PARAMS[10].key, disp, PARAMS[10].opts[PARAMS[10].nopts - 1]);

    /* float round-trip: a_density (index 2) */
    a->setP(a, 2, 0.75f);
    printf("a_density -> 0.75 => norm %.3f\n", a->getP(a, 2));

    /* momentary: a_generate (index 0) fires then springs back via audioMasterAutomate,
     * which this host stub ignores, but setParameter itself must not crash. */
    a->setP(a, 0, 1.0f);

    /* drive transport: playing at 120 BPM, advance ppqPos across many blocks so the
     * synthesized 24-PPQN clock steps the sequencer (acid_process_midi's 0xF8 path). */
    g_ti.flags = kPlaying | kPpq | kTempo;
    g_ti.tempo = 120.0;
    float L[128], R[128], *out[2] = { L, R };
    double sr = 44100.0, ppq_per_block = (g_ti.tempo / 60.0) * (128.0 / sr);
    for (int k = 0; k < 400; k++) {
        a->pr(a, 0, out, 128);
        g_ti.ppqPos += ppq_per_block;
    }
    a->d(a, 7, 0, 0, disp, 0);
    printf("after 400 blocks playing: a_generate display '%s' (should be empty, trigger sprung back)\n", disp);

    /* popup (wrapper/popup.h): a tap opens it, a Q-Link nudge leaves it open, a pick closes it and
     * tells the host once, and the flag stays out of the chunk */
    int fails = 0;
    for (int i = 0; i < NPARAMS; i++) {
        if (PARAMS[i].popup_of < 0) continue;
        int t = PARAMS[i].popup_of, no = PARAMS[t].nopts;
        a->setP(a, i, 1.0f);
        int opened = a->getP(a, i) > 0.5f;
        a->setP(a, t, 0.5f / (no - 1)); a->pr(a, 0, out, 128);
        int kept = a->getP(a, i) > 0.5f && !automated[i];
        a->setP(a, t, 1.0f); a->pr(a, 0, out, 128);
        int closed = a->getP(a, i) < 0.5f && automated[i] == 1;
        printf("popup %s: opens %d, nudge keeps it open %d, pick closes it %d\n", PARAMS[i].key, opened, kept, closed);
        fails += !(opened && kept && closed);
    }


    /* clock (lunarscraper PpqClock fix): pulse N is ppq N/24, fed exactly once -- a host that repeats,
     * overlaps or skips a little ppqPos between blocks (tempo changes, uneven blocks) must neither double
     * nor lose a pulse, and a loop wrap re-anchors without drifting */
    {
        AEffect *g = VSTPluginMain(host);
        double ppb = (120.0 / 60.0) * (128.0 / sr);
        g_ti.tempo = 126.0; g_ti.flags = kPlaying | kPpq | kTempo; g->pr(g, 0, out, 128);
        printf("host tempo 126 -> core bpm %.1f\n", acid_dbg_bpm());
        if (acid_dbg_bpm() < 125.9f || acid_dbg_bpm() > 126.1f) { printf("FAIL host tempo never reached the core (%.1f)\n", acid_dbg_bpm()); fails++; }
        g_ti.tempo = 120.0;
        /* (1) straight run from 0: pulses 0..last fed once each */
        g_ti.flags = 0; g->pr(g, 0, out, 128);
        g_ti.flags = kPlaying | kPpq | kTempo;
        long p0 = acid_dbg_pulses(g);
        double q = 0.0;
        for (int k = 0; k < 2000; k++) { g_ti.ppqPos = q; g->pr(g, 0, out, 128); q += ppb; }
        long got = acid_dbg_pulses(g) - p0, exp = (long)ceil((q - 1e-9) * 24.0 - 1e-6);
        printf("clock: straight run -> %ld pulses (expect %ld)\n", got, exp);
        if (got != exp) { printf("FAIL straight run pulse count\n"); fails++; }
        /* (2) tempo-change jitter: ppqPos steps back 0.1 beat now and then -> nothing fed twice */
        g_ti.flags = 0; g->pr(g, 0, out, 128);
        g_ti.flags = kPlaying | kPpq | kTempo;
        p0 = acid_dbg_pulses(g);
        q = 0.0; double qmax = 0.0, qend = 0.0;
        for (int k = 0; k < 2000; k++) {
            g_ti.ppqPos = q; g->pr(g, 0, out, 128);
            q += ppb; if (q > qmax) qmax = q;
            if (k % 50 == 49) q -= 0.1;
        }
        qend = qmax;
        got = acid_dbg_pulses(g) - p0; exp = (long)ceil((qend - 1e-9) * 24.0 - 1e-6);
        printf("clock: step-backs -> %ld pulses (expect %ld)\n", got, exp);
        if (labs(got - exp) > 0) { printf("FAIL pulse fed twice after ppqPos stepped back\n"); fails++; }
        /* (3) small holes: ppqPos jumps 0.2 beat ahead now and then -> the skipped pulses are caught up */
        g_ti.flags = 0; g->pr(g, 0, out, 128);
        g_ti.flags = kPlaying | kPpq | kTempo;
        p0 = acid_dbg_pulses(g);
        q = 0.0;
        for (int k = 0; k < 2000; k++) {
            g_ti.ppqPos = q; g->pr(g, 0, out, 128);
            q += ppb;
            if (k % 61 == 60) q += 0.2;
        }
        got = acid_dbg_pulses(g) - p0; exp = (long)ceil((q - 1e-9) * 24.0 - 1e-6);
        printf("clock: small holes -> %ld pulses (expect %ld)\n", got, exp);
        if (labs(got - exp) > 0) { printf("FAIL pulse lost in a small hole\n"); fails++; }
        /* (4) 4-beat loop, wrap not on a block boundary, 50 loops: 96 pulses per loop, no drift */
        g_ti.flags = 0; g->pr(g, 0, out, 128);
        g_ti.flags = kPlaying | kPpq | kTempo;
        double abs_ppq = 0, abs5 = 0; long at5 = 0;
        for (long k = 0; abs_ppq < 4.0 * 50; k++) {
            g_ti.ppqPos = fmod(abs_ppq, 4.0);
            g->pr(g, 0, out, 128);
            abs_ppq += ppb;
            if (!at5 && abs_ppq >= 4.0 * 5 + 0.2) { at5 = acid_dbg_pulses(g); abs5 = abs_ppq; }
        }
        long at50 = acid_dbg_pulses(g);
        long lexp = (long)ceil(abs_ppq * 24.0 - 1e-6) - (long)ceil(abs5 * 24.0 - 1e-6);   /* pulses in (abs5, end] of song time */
        printf("clock: 50 loops of 4 beats, measured from beat %.1f -> %ld pulses (expect %ld, within 1)\n", abs5, at50 - at5, lexp);
        if (labs((at50 - at5) - lexp) > 1) { printf("FAIL loop pulse count drifted\n"); fails++; }
        g->d(g, 1, 0, 0, 0, 0);
    }

    /* insert-effect variant: audio passes through, no IsSynth flag, Effect category */
    if (a->ni == 2) {
        float iL[128], iR[128], *in[2] = { iL, iR };
        for (int i = 0; i < 128; i++) { iL[i] = 0.01f * i; iR[i] = -0.01f * i; }
        a->pr(a, in, out, 128);
        int same = 1;
        for (int i = 0; i < 128; i++) if (L[i] != iL[i] || R[i] != iR[i]) same = 0;
        a->pr(a, in, in, 128);   /* in-place, as hosts may do */
        for (int i = 0; i < 128; i++) if (iL[i] != 0.01f * i) same = 0;
        int cat = (int)a->d(a, 35, 0, 0, 0, 0);
        printf("effect: pass-through %d, IsSynth flag %d, category %d (expect 1, 0, 1)\n", same, !!(a->flags & (1 << 8)), cat);
        if (!same || (a->flags & (1 << 8)) || cat != 1) { printf("FAIL insert effect\n"); fails++; }
    } else {
        int cat = (int)a->d(a, 35, 0, 0, 0, 0);
        printf("instrument: IsSynth flag %d, category %d (expect 1, 2)\n", !!(a->flags & (1 << 8)), cat);
        if (!(a->flags & (1 << 8)) || cat != 2) { printf("FAIL instrument\n"); fails++; }
    }

    /* control by MIDI: Control Ch defaults to 16; a CC on it moves the knob, other channels don't,
     * selectors pick an option, triggers fire once per press, OFF ignores everything */
    {
        int icc = pidx("cc_ch"), iden = pidx("a_density"), idir = pidx("a_dir"), ihold = pidx("b_hold");
        char d[64];
        a->d(a, 7, icc, 0, d, 0);
        printf("control ch default '%s' (expect 16)\n", d);
        if (strcmp(d, "16")) { printf("FAIL control ch default\n"); fails++; }
        a->setP(a, iden, 0.5f);
        acid_dbg_control(a, 0xB0, 22, 127);                /* channel 1: not ours */
        float n1 = a->getP(a, iden);
        acid_dbg_control(a, 0xBF, 22, 0);                  /* channel 16, CC 22 = A density */
        float n2 = a->getP(a, iden);
        acid_dbg_control(a, 0xBF, 30, 127);                /* CC 30 = A dir -> last option */
        a->d(a, 7, idir, 0, d, 0);
        acid_dbg_control(a, 0xBF, 53, 127);                /* CC 53 = B hold */
        float n3 = a->getP(a, ihold);
        printf("cc: ch1 ignored %d, density -> %.2f, dir -> %s, b_hold -> %.2f, automate %d\n",
               n1 > 0.49f && n1 < 0.51f, n2, d, n3, automated[iden] > 0);
        if (!(n1 > 0.49f && n1 < 0.51f) || n2 > 0.01f || strcmp(d, "PENDUL") || n3 < 0.99f || !automated[iden]) { printf("FAIL cc control\n"); fails++; }
        char before[2048], after[2048];
        void *ck = 0;
        intptr_t cn = a->d(a, 23, 0, 0, &ck, 0); (void)cn;
        const char *dz = strstr((char *)ck, "a_dice="); snprintf(before, sizeof before, "%.1500s", dz ? dz : "");
        acid_dbg_control(a, 0xBF, 20, 127);                /* generate: rising edge fires */
        acid_dbg_control(a, 0xBF, 20, 127);                /* held: must not fire again */
        a->d(a, 23, 0, 0, &ck, 0);
        dz = strstr((char *)ck, "a_dice="); snprintf(after, sizeof after, "%.1500s", dz ? dz : "");
        int gen1 = strcmp(before, after) != 0;
        a->d(a, 23, 0, 0, &ck, 0);
        acid_dbg_control(a, 0xBF, 20, 0);
        a->setP(a, icc, 0.0f);                             /* OFF */
        acid_dbg_control(a, 0xBF, 22, 127);
        float n4 = a->getP(a, iden);
        a->setP(a, icc, 3.0f / 16.0f);                     /* channel 3 */
        a->d(a, 23, 0, 0, &ck, 0);
        int saved = strstr((char *)ck, "cc_ch=3;") != NULL;
        printf("cc: generate fired %d, OFF ignores %d, cc_ch saved %d\n", gen1, n4 < 0.01f, saved);
        if (!gen1 || n4 > 0.01f || !saved) { printf("FAIL cc trigger/off/save\n"); fails++; }
        a->setP(a, icc, 1.0f);
    }

    /* HOLD / SHIFT / VEL RAND (engine level): same seed, one knob changed at a time */
    {
        midi_fx_api_v1_t *api = move_midi_fx_init(&T_HOST);
        static ev_t e0[4000], e1[4000];
        #define BASE "a_seed", "4242", "a_density", "0.5", "a_slide", "0", "a_accent", "0.3", "blend", "-63", "b_density", "0"
        const char *const k0[] = { BASE, NULL };
        const char *const kh[] = { BASE, "a_hold", "1", NULL };
        const char *const ks[] = { BASE, "a_shift", "7", NULL };   /* 7 degrees of a 7-note scale = one octave */
        const char *const kv[] = { BASE, "a_velrand", "1", NULL };
        int n0 = run_core(api, k0, 64, e0, 4000);
        /* hold 0: every note ends in its own step */
        int long0 = 0, on0 = 0;
        for (int i = 0; i < n0; i++) if ((e0[i].m[0] & 0xF0) == 0x90 && e0[i].m[2]) {
            on0++;
            for (int j = i + 1; j < n0; j++)
                if (((e0[j].m[0] & 0xF0) == 0x80 || ((e0[j].m[0] & 0xF0) == 0x90 && !e0[j].m[2])) && e0[j].m[1] == e0[i].m[1]) {
                    if (e0[j].step > e0[i].step) long0++;
                    break;
                }
        }
        int n1 = run_core(api, kh, 64, e1, 4000), long1 = 0, on1 = 0, overlap = 0, sounding = 0;
        for (int i = 0; i < n1; i++) {
            uint8_t t = e1[i].m[0] & 0xF0;
            if (t == 0x90 && e1[i].m[2]) {
                on1++;
                if (sounding) overlap++;
                sounding = 1;
                for (int j = i + 1; j < n1; j++)
                    if (((e1[j].m[0] & 0xF0) == 0x80 || ((e1[j].m[0] & 0xF0) == 0x90 && !e1[j].m[2])) && e1[j].m[1] == e1[i].m[1]) {
                        if (e1[j].step > e1[i].step) long1++;
                        break;
                    }
            } else if (t == 0x80 || (t == 0x90 && !e1[i].m[2])) sounding = 0;
        }
        printf("hold: notes %d/%d, longer than a 16th: hold 0 -> %d, hold 1 -> %d, overlaps %d\n", on0, on1, long0, long1, overlap);
        if (on0 != on1 || long0 != 0 || long1 == 0 || overlap) { printf("FAIL hold\n"); fails++; }
        /* shift: +7 degrees = every note one octave up, nothing else changed */
        int ns = run_core(api, ks, 64, e1, 4000), sh_ok = ns == n0;
        for (int i = 0; sh_ok && i < n0; i++) {
            uint8_t t = e0[i].m[0] & 0xF0;
            if (t == 0x90 || t == 0x80) sh_ok = e1[i].m[1] == e0[i].m[1] + 12;
        }
        printf("shift +7 degrees = +12 semitones on every note: %d\n", sh_ok);
        if (!sh_ok) { printf("FAIL shift\n"); fails++; }
        /* vel rand: velocities spread around 72/118 instead of being exactly those */
        int nv = run_core(api, kv, 64, e1, 4000), plain0 = 1, distinct = 0, seen[128] = {0};
        for (int i = 0; i < n0; i++) if ((e0[i].m[0] & 0xF0) == 0x90 && e0[i].m[2]) plain0 &= (e0[i].m[2] == 72 || e0[i].m[2] == 118);
        for (int i = 0; i < nv; i++) if ((e1[i].m[0] & 0xF0) == 0x90 && e1[i].m[2] && !seen[e1[i].m[2]]++) distinct++;
        printf("vel rand: off -> only 72/118 %d, on -> %d distinct velocities\n", plain0, distinct);
        if (!plain0 || distinct < 6) { printf("FAIL vel rand\n"); fails++; }
    }

    /* chunk round-trip */
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    printf("chunk %ld bytes: %.120s...\n", (long)n, (char *)chunk);
    b->d(b, 24, 0, n, chunk, 0);
    printf("b after setChunk: a_dir norm %.3f (a %.3f)\n", b->getP(b, 10), a->getP(a, 10));
    if (strstr((char *)chunk, "__open")) { printf("FAIL popup flag saved in the chunk\n"); fails++; }

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
