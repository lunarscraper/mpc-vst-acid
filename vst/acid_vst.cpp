/* =============================================================================
 * acid_vst.cpp - Acid as a VST2 MIDI generator for the MPC OS plugin host
 * (see https://github.com/sd88me/mpc-vst-plugins). Links src/acid_core.c
 * directly, the same generator src/host_shim.cpp drives on MockbaMod, and
 * plays the same "chain host" role host_shim.cpp plays there -- but inside
 * MPC's own JUCE plugin host instead of a standalone process:
 *
 *   host_shim.cpp (MockbaMod)              acid_vst.cpp (MPC plugin)
 *   ------------------------------------   ---------------------------------
 *   RtMidi virtual port, physical clock    audioMasterGetTime (ppqPos/tempo)
 *   in from Force transport                synthesises the same 24-PPQN
 *                                           0xF8/0xFA/0xFC stream from it
 *   CC on a control channel -> set_param   VST parameters (params.h) -> set_param
 *   RtMidi virtual port out                ALSA seq port out (MPC OS ignores a
 *                                           plugin's VST MIDI output -- see
 *                                           mpc-vst-plugins docs/NOTES.md)
 *
 * MPC's transport is a single shared clock, so host_get_bpm/host_get_clock_status
 * (acid_core.h) stay process-wide globals same as host_shim.cpp's -- every
 * instance's process_midi/tick calls still go through its own acid_inst_t.
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

extern "C" {
#include "acid_core.h"
}
#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[64]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

/* ---------------------------------------------------------------------------
 * Process-wide host callbacks -- MPC has one transport for every track/plugin,
 * same simplification host_shim.cpp makes (acid_core.h's host_api_v1_t takes
 * no instance argument). Updated from whichever instance's process() runs
 * most recently; every instance sees the same MPC playhead anyway.
 * ------------------------------------------------------------------------- */
static std::atomic<float> g_bpm{120.0f};
static std::atomic<int>   g_clock_status{MOVE_CLOCK_STATUS_STOPPED};
static float host_get_bpm(void) { return g_bpm.load(); }
static int   host_get_clock_status(void) { return g_clock_status.load(); }
static const host_api_v1_t g_host = { host_get_bpm, host_get_clock_status };
static midi_fx_api_v1_t *g_api = nullptr;
static std::mutex g_api_init_lock;
/* Port names by lowest free slot, given back on close. A plain counter never went down, so
 * an instance the host re-created (removing and re-adding the plugin, possibly loading a
 * preset) came back as "Acid 2" and the synth track listening on "Acid" went silent. */
static std::mutex g_slot_lock;
static bool g_slot_used[32];
static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

/* ---------------------------------------------------------------------------
 * PpqClock: host ppqPos -> 24-PPQN pulses by ABSOLUTE pulse index.
 *
 * The first version fed "every pulse inside this block's own window
 * [ppqPos, ppqPos + block length)". That is only exact-once if every callback's
 * ppqPos advances by precisely frames*tempo/60/sr. When it doesn't (host splits
 * or repeats a block, reports ppqPos late, re-derives it on a tempo change,
 * restarts without dropping the playing flag) windows overlap or leave holes:
 * pulses get fed twice or never, and since both engines COUNT pulses, every
 * such pulse shifts the pattern against the MPC grid for good -> audible as an
 * unstable tempo.
 *
 * Here pulse N simply IS ppq N/24. We remember the last index fed and feed
 * (sent, last-in-this-block]: an overlap feeds nothing twice, a small hole is
 * caught up at once, and a real jump (loop, locate, restart) is detected and
 * re-anchored instead of silently drifting.
 * ------------------------------------------------------------------------- */
struct PpqClock {
    long long sent = 0;        /* absolute index of the last pulse fed */
    bool resync = true;        /* next block re-anchors (transport start) */
    bool wait = false;         /* phase_mod only: holding until the old phase comes round again */
    int phase = 0;
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;   /* diagnostics */
};
static const long long PPQ_JUMP_TOL = 12;   /* pulses (half a beat): beyond this it's a locate, not jitter */
static inline long long ppq_mod(long long a, long long m) { long long r = a % m; return r < 0 ? r + m : r; }

/* One audio block. Returns true and sets [*from, *to] (inclusive pulse indices) if pulses are due.
 * phase_mod 0: caller can place the engine at any index (Euclidier sets its tick = index).
 * phase_mod N: the engine only counts pulses (Acid, 6 per step, 12 per swing pair); after a jump
 *              that isn't a multiple of N, hold until the index is back in the old phase, so the
 *              pattern carries on ON the MPC grid instead of beside it. */
static inline bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                                   int phase_mod, long long *from, long long *to, bool *jumped) {
    double end = ppq + frames * (tempo / 60.0) / sr;
    long long first = (long long)std::ceil(ppq * 24.0 - 1e-6);       /* first pulse at/after block start */
    long long last = (long long)std::ceil(end * 24.0 - 1e-6) - 1;    /* last pulse before block end */
    *jumped = false;
    if (c->resync) {
        c->sent = first - 1; c->resync = false; c->wait = false;
    } else if (!c->wait && (first > c->sent + 1 + PPQ_JUMP_TOL || last < c->sent - PPQ_JUMP_TOL)) {
        *jumped = true; c->jumps++;
        if (phase_mod > 0 && ppq_mod(first - (c->sent + 1), phase_mod) != 0) {
            c->wait = true; c->phase = (int)ppq_mod(c->sent + 1, phase_mod);
        } else c->sent = first - 1;
    }
    if (c->wait) {
        long long k = first + ppq_mod(c->phase - first, phase_mod);   /* next index in the old phase */
        if (k > last) return false;
        c->sent = k - 1; c->wait = false;
    }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);                       /* hole: caught up now */
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;   /* overlap: not fed twice */
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

#define BUILD_ID "presets32-2026-10-03"

/* ---------------------------------------------------------------------------
 * Preset bank: NSLOTS slots shared by every Acid instance and every project,
 * kept in one text file on the SD card (one line per used slot:
 * "index<TAB>name<TAB>chunk"). A slot holds the same "key=value;" chunk a
 * project saves: all knobs plus both sequences. The slots are also the
 * plugin's VST programs, so the host's PRESET list shows and selects them;
 * the PRESET knob + LOAD/SAVE buttons on the GLOBAL page do the same from
 * the skin, whatever the host does with programs.
 * ------------------------------------------------------------------------- */
#define NSLOTS 32
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

/* Next to the plugin's own folder rather than inside it, so reinstalling the plugin folder
 * doesn't take the presets with it; inside it if the parent can't be written. No dladdr
 * (would need -ldl on the older glibc this is built against): /proc/self/maps has the path. */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/acid.so");
            char *start = std::strchr(line, '/');
            if (!p || !start || start > p) continue;
            dir.assign(start, (size_t)(p - start));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    std::string dir = so_dir(), cand[2];
    if (dir.empty()) dir = "/tmp";
    size_t cut = dir.rfind('/');
    cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/acid_presets.txt";
    cand[1] = dir + "/acid_presets.txt";
    g_bank_path.clear();
    for (const std::string &c : cand)             /* an existing file wins ... */
        if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
    for (int i = 0; i < 2 && g_bank_path.empty(); i++)   /* ... else the first place we may write */
        if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    if (g_bank_path.empty()) { LOG("[acid_vst] presets: no writable place near %s\n", dir.c_str()); return; }
    int used = 0;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[9000];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
            used++;
        }
        std::fclose(f);
    }
    LOG("[acid_vst] presets: %s, %d of %d slots used\n", g_bank_path.c_str(), used, NSLOTS);
}
static bool bank_write_locked() {   /* whole file, via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) { LOG("[acid_vst] presets: cannot write %s\n", tmp.c_str()); return false; }
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    bool ok = std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
    if (!ok) LOG("[acid_vst] presets: write to %s failed\n", g_bank_path.c_str());
    return ok;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) { std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1); return buf; }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "Acid %02d", i + 1);
    return buf;
}

/* ---------------------------------------------------------------------------
 * Per-instance state
 * ------------------------------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    void *inst = nullptr;
    std::mutex lock;               /* serialises every call into the core */
    volatile int release[NPARAMS] = {0};
    bool held[NPARAMS] = {false};  /* momentary params: host currently reports them pressed */
    float open[NPARAMS] = {0};     /* popup "open" flags (popup.h): wrapper-only, not saved */
    double last_ppq = 0.0;
    bool was_playing = false;
    PpqClock clk;
    int jump_logs = 0, param_logs = 0;   /* log budget, refilled with every 5 s stats line */
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    int cur = 0;                   /* selected preset slot, 0-based */
    int slot = -1;                 /* ALSA port name slot: 0 = "Acid", 1 = "Acid 2", ... */
    char chunk[8192] = {0};        /* params + both sequences (a_dice/b_dice, ~1.3 kB each) */
    /* diagnostics: per-callback timing, logged periodically and on slow callbacks */
    double t_max = 0, t_sum = 0, t_last_log = 0;
    long blocks = 0, slow = 0;
    int32_t block_min = 1 << 30, block_max = 0;
    /* loop guard: MPC record-arm/input-monitor can route this plugin's own
       ALSA output straight back in as VstMidiEvents (effProcessEvents), and
       the core's "pass everything else through" branch echoes it straight
       back out -- a self-sustaining feedback loop, audio-thread only so no
       lock needed. Remember the last few bytes we actually sent and drop an
       incoming event that matches one, once, instead of re-feeding it. */
    uint8_t sent_ring[16][3] = {{0}};
    int sent_len = 0, sent_pos = 0;
};
static bool was_just_sent(Plugin *w, const uint8_t *m) {
    /* Match on status+data1 only, NOT velocity (data2): verified 2026-09-27
     * that MPC's record-arm loopback doesn't return our own output byte-exact
     * -- velocity gets quantized in transit (e.g. sent 72 comes back as 71,
     * a note-off's velocity 0 comes back as 64), so an exact 3-byte compare
     * missed most echoes and let them fall through as "real" transpose
     * input, continuously re-transposing the live pattern off its own
     * output. A genuine human keypress landing on the exact same note
     * number as the sequencer's own output, in the very same instant, isn't
     * a realistic collision to worry about. */
    for (int i = 0; i < w->sent_len; i++) {
        int idx = (w->sent_pos - 1 - i + 16) % 16;
        if (w->sent_ring[idx][0] == m[0] && w->sent_ring[idx][1] == m[1]) {
            w->sent_ring[idx][0] = 0xFF;   /* consume: don't match it again */
            return true;
        }
    }
    return false;
}
static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* normalized 0..1 <-> core value string, same convention as wrapper/vst2_wrap.c */
static void norm_to_str(const param_t *p, float n, char *buf, int len) {
    if (p->nopts) std::snprintf(buf, len, "%d", (int)std::lround(clamp01(n) * (p->nopts - 1)));
    else std::snprintf(buf, len, "%g", p->min + (p->max - p->min) * clamp01(n));
}
static float str_to_norm(const param_t *p, const char *s) {
    if (p->nopts) {
        int idx = std::atoi(s);
        if (idx < 0) idx = 0;
        if (idx > p->nopts - 1) idx = p->nopts - 1;
        return p->nopts > 1 ? (float)idx / (p->nopts - 1) : 0.0f;
    }
    return p->max > p->min ? clamp01((float)((std::atof(s) - p->min) / (p->max - p->min))) : 0.0f;
}
static int g_i_slot = -1, g_i_load = -1, g_i_save = -1;   /* wrapper-only params (module.json: slot/load/save) */
static void find_preset_params() {
    for (int i = 0; i < NPARAMS; i++) {
        if (!std::strcmp(PARAMS[i].key, "slot")) g_i_slot = i;
        else if (!std::strcmp(PARAMS[i].key, "load")) g_i_load = i;
        else if (!std::strcmp(PARAMS[i].key, "save")) g_i_save = i;
    }
}
static float get_norm(Plugin *w, int i) {
    char buf[64];
    int n;
    if (i == g_i_slot) return (float)w->cur / (NSLOTS - 1);
    if (popup_is(i)) return w->open[i];
    if (PARAMS[i].momentary) return 0.0f;   /* triggers always read released, or the host echoes the press back */
    { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, PARAMS[i].key, buf, sizeof buf); }
    return n > 0 ? str_to_norm(&PARAMS[i], buf) : PARAMS[i].def;
}

/* ---------------------------------------------------------------------------
 * ALSA seq output -- MPC OS ignores a plugin's VST MIDI output (mpc-vst-plugins
 * docs/NOTES.md), so notes go out a real ALSA sequencer port instead, exactly
 * as poc/midiport.c verified. MPC hot-detects the port with no restart; the
 * user routes a track's MIDI input from it, one per Acid instance.
 * ------------------------------------------------------------------------- */
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { w->seq = nullptr; return; }
    snd_seq_set_client_pool_output(w->seq, 2048);   /* headroom so a slow subscriber never stalls the audio thread */
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_slot_lock);
        while (n < 31 && g_slot_used[n]) n++;
        g_slot_used[n] = true;
        w->slot = n;
    }
    char name[32];
    if (n == 0) std::snprintf(name, sizeof name, "Acid");
    else std::snprintf(name, sizeof name, "Acid %d", n + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[acid_vst] ALSA client '%s' port %d\n", name, w->seq_port);
}
static void alsa_send(Plugin *w, const uint8_t (*msgs)[3], const int *lens, int n) {
    for (int i = 0; i < n; i++) {
        if (lens[i] < 2) continue;
        w->sent_ring[w->sent_pos][0] = msgs[i][0];
        w->sent_ring[w->sent_pos][1] = msgs[i][1];
        w->sent_ring[w->sent_pos][2] = lens[i] > 2 ? msgs[i][2] : 0;
        w->sent_pos = (w->sent_pos + 1) % 16;
        if (w->sent_len < 16) w->sent_len++;
    }
    if (!w->seq || w->seq_port < 0) return;
    for (int i = 0; i < n; i++) {
        const uint8_t *m = msgs[i];
        int len = lens[i];
        if (len < 2) continue;
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, w->seq_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        uint8_t type = m[0] & 0xF0, ch = m[0] & 0x0F;
        if (type == 0x90 && len >= 3 && m[2] > 0) snd_seq_ev_set_noteon(&ev, ch, m[1], m[2]);
        else if (type == 0x80 || (type == 0x90 && len >= 3)) snd_seq_ev_set_noteoff(&ev, ch, m[1], 0);
        else if (type == 0xB0 && len >= 3) snd_seq_ev_set_controller(&ev, ch, m[1], m[2]);
        else continue;
        snd_seq_event_output(w->seq, &ev);   /* buffered, non-blocking: drops instead of hanging */
    }
    snd_seq_drain_output(w->seq);
}
static void alsa_close(Plugin *w) {
    if (!w->seq) return;
    for (int ch = 0; ch < 16; ch++)
        for (int note = 0; note < 128; note++) {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, w->seq_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_noteoff(&ev, ch, note, 0);
            snd_seq_event_output_direct(w->seq, &ev);
        }
    snd_seq_close(w->seq);
    w->seq = nullptr;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    if (w->slot >= 0) { g_slot_used[w->slot] = false; w->slot = -1; }
}

/* ---------------------------------------------------------------------------
 * Transport / clock synthesis: audioMasterGetTime -> synthetic 24-PPQN clock,
 * the same byte stream host_shim.cpp fed acid_core.c from real MIDI clock.
 * Runs once per audio block (unlocked call into process_midi per pulse).
 * ------------------------------------------------------------------------- */
static void feed_transport(Plugin *w, int32_t frames) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0,
                                                kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo > 0) g_bpm.store((float)ti->tempo);

    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
    int olen[MIDI_FX_MAX_OUT_MSGS];
    uint8_t msg[1];

    if (playing && !w->was_playing) {
        g_clock_status.store(MOVE_CLOCK_STATUS_RUNNING);
        w->last_ppq = ti->ppqPos;
        w->clk.resync = true;   /* anchor the pulse index on this block */
        msg[0] = 0xFA;
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_api->process_midi(w->inst, msg, 1, out, olen, MIDI_FX_MAX_OUT_MSGS); }
        alsa_send(w, out, olen, n);
    } else if (!playing && w->was_playing) {
        g_clock_status.store(MOVE_CLOCK_STATUS_STOPPED);
        msg[0] = 0xFC;
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_api->process_midi(w->inst, msg, 1, out, olen, MIDI_FX_MAX_OUT_MSGS); }
        alsa_send(w, out, olen, n);
    }
    w->was_playing = playing;

    if (playing && ti && ti->tempo > 0) {
        /* Pulses by absolute index (PpqClock above): exact-once whatever the host does with
         * ppqPos between callbacks. 12 = one swing pair of 16ths in 24-PPQN pulses; the core
         * only counts pulses, so after a jump its phase has to be carried over. */
        double sr = (ti->sampleRate > 0) ? ti->sampleRate : (double)MOVE_SAMPLE_RATE;
        long long from = 0, to = -1;
        bool jumped = false;
        long long before = w->clk.sent;
        bool due = ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, frames, 12, &from, &to, &jumped);
        if (jumped && w->jump_logs < 20) {
            w->jump_logs++;
            LOG("[acid_vst] ppq jump: last pulse %lld -> ppq %.4f (pulse %.2f), bpm %.2f, block %d%s\n",
                before, ti->ppqPos, ti->ppqPos * 24.0, ti->tempo, frames, w->clk.wait ? ", re-phasing" : "");
        }
        for (long long idx = from; due && idx <= to; idx++) {
            msg[0] = 0xF8;
            int n;
            { std::lock_guard<std::mutex> lk(w->lock); n = g_api->process_midi(w->inst, msg, 1, out, olen, MIDI_FX_MAX_OUT_MSGS); }
            alsa_send(w, out, olen, n);
        }
        w->last_ppq = ti->ppqPos;
    }

    int n;
    { std::lock_guard<std::mutex> lk(w->lock); n = g_api->tick(w->inst, frames, MOVE_SAMPLE_RATE, out, olen, MIDI_FX_MAX_OUT_MSGS); }
    alsa_send(w, out, olen, n);
}

/* ---------------------------------------------------------------------------
 * State as text: "key=value;" for every knob, then both sequences. Used for the
 * project/preset chunk and for the preset slots alike.
 * ------------------------------------------------------------------------- */
static std::string build_state(Plugin *w) {
    std::string s;
    for (int i = 0; i < NPARAMS; i++) {
        if (PARAMS[i].momentary || popup_is(i) || i == g_i_slot) continue;
        char buf[32];
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, PARAMS[i].key, buf, sizeof buf); }
        if (n <= 0) continue;
        buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
        s += PARAMS[i].key; s += '='; s += buf; s += ';';
    }
    /* the sequences themselves, after the knobs so they are restored last */
    for (const char *key : {"a_dice", "b_dice"}) {
        char big[2048];
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, key, big, sizeof big); }
        if (n > 0) { s += key; s += '='; s += big; s += ';'; }
    }
    return s;
}
static void apply_state(Plugin *w, std::string s, bool with_slot) {   /* by value: strtok_r cuts it up */
    std::lock_guard<std::mutex> lk(w->lock);
    char *save = nullptr;
    for (char *tok = strtok_r(&s[0], ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        char *eq = std::strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (!std::strcmp(tok, "prog")) {   /* which slot a project had selected; never loads the slot */
            int v = std::atoi(eq + 1);
            if (with_slot && v >= 0 && v < NSLOTS) w->cur = v;
            continue;
        }
        g_api->set_param(w->inst, tok, eq + 1);
    }
}
static void slot_load(Plugin *w) {
    std::string c;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[w->cur]; }
    LOG("[acid_vst] %p load preset %d (%zu bytes)\n", (void *)w, w->cur + 1, c.size());
    if (!c.empty()) apply_state(w, c, false);   /* an empty slot leaves the current state alone */
}
static void slot_save(Plugin *w) {
    std::string c = build_state(w);
    bool ok;
    { std::lock_guard<std::mutex> lk(g_bank_lock); g_slot_chunk[w->cur] = c; ok = bank_write_locked(); }
    LOG("[acid_vst] %p save preset %d (%zu bytes) %s\n", (void *)w, w->cur + 1, c.size(), ok ? "ok" : "FAILED");
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    double t0 = now_ms();
    feed_transport(w, n);
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->held[i] = false; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */

    double t1 = now_ms(), dt = t1 - t0;
    w->blocks++; w->t_sum += dt;
    if (dt > w->t_max) w->t_max = dt;
    if (n < w->block_min) w->block_min = n;
    if (n > w->block_max) w->block_max = n;
    if (dt > 2.0) { w->slow++; LOG("[acid_vst] SLOW callback %.2f ms (block %d, ppq %.3f)\n", dt, n, w->last_ppq); }
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[acid_vst] %ld blocks, size %d..%d, avg %.3f ms, max %.2f ms, slow %ld, playing %d, ppq %.3f, bpm %.2f"
                " | clock: %ld pulses, %ld caught up, %ld overlapped, %ld jumps\n",
                w->blocks, w->block_min, w->block_max, w->t_sum / w->blocks, w->t_max, w->slow,
                (int)w->was_playing, w->last_ppq, g_bpm.load(),
                w->clk.pulses, w->clk.gaps, w->clk.dups, w->clk.jumps);
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->jump_logs = w->param_logs = 0;
        w->t_last_log = t1; w->t_max = w->t_sum = 0; w->blocks = w->slow = 0;
        w->block_min = 1 << 30; w->block_max = 0;
    }
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (w->param_logs < 40) {   /* diagnostics: does the host's control change reach this instance at all? */
        w->param_logs++;
        LOG("[acid_vst] %p setParameter %d (%s) = %.3f\n", (void *)w, i, p->key, n);
    }
    if (popup_set(w->open, i, n)) return;
    if (i == g_i_slot) {   /* browsing only: LOAD loads, so turning the knob can't wipe what is playing */
        int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur) { w->cur = v; w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (i == g_i_load || i == g_i_save) {
        bool down = n > 0.5f;
        bool rising = down && !w->held[i];
        w->held[i] = down;
        if (rising) {
            if (i == g_i_load) slot_load(w); else slot_save(w);
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    if (p->momentary) {
        bool down = n > 0.5f;
        bool rising = down && !w->held[i];   /* an echo of our own automate must not re-fire the trigger */
        w->held[i] = down;
        if (rising) {
            std::lock_guard<std::mutex> lk(w->lock);
            g_api->set_param(w->inst, p->key, "go");
            w->release[i] = 1;
        }
        return;
    }
    char buf[32];
    bool nudge = false;
    if (p->nopts > 1) {
        /* An exact option value selects it; anything between options is a
         * Q-Link/encoder nudge from the current one -- step one option that
         * way, same convention as wrapper/vst2_wrap.c. */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = get_norm(w, i) * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    norm_to_str(p, n, buf, sizeof buf);
    {
        std::lock_guard<std::mutex> lk(w->lock);
        g_api->set_param(w->inst, p->key, buf);
    }
    if (!nudge) popup_picked(w->open, w->release, i);   /* a list pick closes it; a Q-Link nudge doesn't */
}

static float getParameter(AEffect *e, int32_t i) { return get_norm((Plugin *)e->object, i); }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        LOG("[acid_vst] closed, instance %p\n", (void *)w);
        alsa_close(w);
        g_api->destroy_instance(w->inst);
        delete w;
        return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) {
            w->cur = (int)v;
            slot_load(w);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur;
    case effGetProgramName: copy_str(p, slot_title(w->cur), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS) return 0;
        copy_str(p, slot_title(idx), 24);
        return 1;
    case effSetProgramName: {   /* only a used slot has a line in the file to carry the name */
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[w->cur].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[w->cur] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 2; /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const param_t *pp = &PARAMS[idx];
        if (pp->momentary) { copy_str(p, "", 24); return 1; }
        if (idx == g_i_slot) { copy_str(p, slot_title(w->cur), 24); return 1; }
        if (pp->nopts) {
            int k = (int)std::lround(get_norm(w, idx) * (pp->nopts - 1));
            copy_str(p, pp->opts[k], 24);
        } else {
            char buf[32];
            int n;
            { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, pp->key, buf, sizeof buf); }
            if (n > 0) {
                /* always show a whole number: a small physical range (density/accent/slide/gate/
                 * jitter, 0..1-ish) reads as its 0-100 percentage instead of a raw decimal; a wide
                 * range (channel, algo, octaves, length, offset, swing, ...) just rounds. */
                float raw = (float)std::atof(buf), range = pp->max - pp->min;
                long show = std::lround(range > 0 && range <= 2.0f ? (raw - pp->min) / range * 100.0f : raw);
                char disp[32];
                std::snprintf(disp, sizeof disp, "%ld", show);
                copy_str(p, disp, 24);
            }
        }
        return 1;
    }
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
        int olen[MIDI_FX_MAX_OUT_MSGS];
        for (int i = 0; i < ev->numEvents; i++) {
            if (ev->events[i]->type != 1) continue;
            VstMidiEvent *m = (VstMidiEvent *)ev->events[i];
            /* MPC's record-arm input monitor loops this plugin's own ALSA
             * output straight back in as VstMidiEvents -- for note-on/off
             * that's not just a no-op echo (the core always swallows note
             * in/out, never re-emits it): it silently re-sets live_transpose
             * from whatever pitch the echoed note carries, permanently
             * detuning the live pattern (verified 2026-09-27). Channel-based
             * filtering (treat any note on a_channel/b_channel as always our
             * own) is too broad -- the user's real transpose input arrives
             * on that same channel (both default to 1), so it silently ate
             * every real transpose press too. Byte-exact match against what
             * Acid actually just sent is the correct discriminator: an echo
             * of our own output matches status+note+velocity exactly, while
             * a human keypress almost never lands on Acid's own fixed
             * generator velocities (72/118 normal, 1/127 CV mode). */
            if (was_just_sent(w, m->midiData)) continue;
            int n;
            { std::lock_guard<std::mutex> lk(w->lock);
              n = g_api->process_midi(w->inst, m->midiData, 3, out, olen, MIDI_FX_MAX_OUT_MSGS); }
            alsa_send(w, out, olen, n);
        }
        return 1;
    }
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstEvents") || !std::strcmp((char *)p, "receiveVstMidiEvent") ||
                !std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        std::string st = build_state(w);
        char prog[24];
        std::snprintf(prog, sizeof prog, "prog=%d;", w->cur);
        st += prog;
        copy_str(w->chunk, st, sizeof w->chunk);
        *(void **)p = w->chunk;
        LOG("[acid_vst] %p getChunk %zu bytes\n", (void *)w, std::strlen(w->chunk) + 1);
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v >= sizeof w->chunk || !p) return 0;
        LOG("[acid_vst] %p setChunk %ld bytes\n", (void *)w, (long)v);
        std::string st((const char *)p, (size_t)v);
        st.resize(std::strlen(st.c_str()));   /* stop at a NUL the host may have included */
        apply_state(w, st, true);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/acid_vst.log", "a");
    {
        std::lock_guard<std::mutex> lk(g_api_init_lock);
        if (!g_api) {
            g_api = move_midi_fx_init(&g_host);
            if (!g_api || g_api->api_version != MIDI_FX_API_VERSION) { LOG("[acid_vst] core init failed\n"); g_api = nullptr; return nullptr; }
        }
    }
    Plugin *w = new Plugin();
    w->master = master;
    {
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (g_i_slot < 0) find_preset_params();
        bank_load_locked();
    }
    w->inst = g_api->create_instance(".", nullptr);
    if (!w->inst) { LOG("[acid_vst] create_instance failed\n"); delete w; return nullptr; }
    alsa_open(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[acid_vst] up (" BUILD_ID "), %d params, instance %p\n", NPARAMS, (void *)w);
    return e;
}
