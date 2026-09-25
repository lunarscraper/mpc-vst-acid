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

static TI g_ti;
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)idx; (void)v; (void)p; (void)o;
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

    /* enum round-trip: a_dir (index 10) -> REV */
    a->setP(a, 10, 1.0f);
    a->d(a, 7, 10, 0, disp, 0);
    printf("a_dir -> 1.0 => %s (expect REV)\n", disp);

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

    /* chunk round-trip */
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    printf("chunk %ld bytes: %.120s...\n", (long)n, (char *)chunk);
    b->d(b, 24, 0, n, chunk, 0);
    printf("b after setChunk: a_dir norm %.3f (a %.3f)\n", b->getP(b, 10), a->getP(a, 10));

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
    printf("OK\n");
    return 0;
}
