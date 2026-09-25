/* =============================================================================
 * acid_vst.cpp - Force Acid as a VST2 MIDI generator for the MPC OS plugin host
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
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

extern "C" {
#include "acid_core.h"
}
#include "params.h"

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
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
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
static std::atomic<int> g_instance_count{0};
static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

/* ---------------------------------------------------------------------------
 * Per-instance state
 * ------------------------------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    void *inst = nullptr;
    std::mutex lock;               /* serialises every call into the core */
    volatile char release[NPARAMS] = {0};
    double last_ppq = 0.0;
    bool was_playing = false;
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    char chunk[2048] = {0};
};

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
static float get_norm(Plugin *w, int i) {
    char buf[64];
    int n;
    { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, PARAMS[i].key, buf, sizeof buf); }
    return n > 0 ? str_to_norm(&PARAMS[i], buf) : PARAMS[i].def;
}

/* ---------------------------------------------------------------------------
 * ALSA seq output -- MPC OS ignores a plugin's VST MIDI output (mpc-vst-plugins
 * docs/NOTES.md), so notes go out a real ALSA sequencer port instead, exactly
 * as poc/midiport.c verified. MPC hot-detects the port with no restart; the
 * user routes a track's MIDI input from it, one per Force Acid instance.
 * ------------------------------------------------------------------------- */
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { w->seq = nullptr; return; }
    int n = g_instance_count.fetch_add(1);
    char name[32];
    if (n == 0) std::snprintf(name, sizeof name, "Force Acid");
    else std::snprintf(name, sizeof name, "Force Acid %d", n + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[acid_vst] ALSA client '%s' port %d\n", name, w->seq_port);
}
static void alsa_send(Plugin *w, const uint8_t (*msgs)[3], const int *lens, int n) {
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
        snd_seq_event_output_direct(w->seq, &ev);
    }
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

    if (playing && ti) {
        const double step = 1.0 / 24.0;   /* 24 PPQN, in quarter notes */
        double start = w->last_ppq, end = ti->ppqPos;
        if (end < start) start = end;     /* loop/rewind: resync, don't flood pulses */
        double next = std::ceil(start / step) * step;
        for (; next < end + 1e-9; next += step) {
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
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    feed_transport(w, n);
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (p->momentary) {
        if (n > 0.5f) {
            std::lock_guard<std::mutex> lk(w->lock);
            g_api->set_param(w->inst, p->key, "go");
            w->release[i] = 1;
        }
        return;
    }
    char buf[32];
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
        }
    }
    norm_to_str(p, n, buf, sizeof buf);
    std::lock_guard<std::mutex> lk(w->lock);
    g_api->set_param(w->inst, p->key, buf);
}

static float getParameter(AEffect *e, int32_t i) { return get_norm((Plugin *)e->object, i); }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        alsa_close(w);
        g_api->destroy_instance(w->inst);
        delete w;
        return 1;
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
        if (pp->nopts) {
            int k = (int)std::lround(get_norm(w, idx) * (pp->nopts - 1));
            copy_str(p, pp->opts[k], 24);
        } else {
            char buf[32];
            int n;
            { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, pp->key, buf, sizeof buf); }
            if (n > 0) {
                char disp[32];
                std::snprintf(disp, sizeof disp, "%.*f", std::fabs(pp->max - pp->min) > 20 ? 0 : 2, std::atof(buf));
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
        std::string s;
        for (int i = 0; i < NPARAMS; i++) {
            if (PARAMS[i].momentary) continue;
            char buf[32];
            int n;
            { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, PARAMS[i].key, buf, sizeof buf); }
            if (n <= 0) continue;
            buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
            s += PARAMS[i].key; s += '='; s += buf; s += ';';
        }
        copy_str(w->chunk, s, sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::memcpy(w->chunk, p, (size_t)v);
        w->chunk[v - 1] = 0;
        std::lock_guard<std::mutex> lk(w->lock);
        char *s = w->chunk, *save = nullptr;
        for (char *tok = strtok_r(s, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            g_api->set_param(w->inst, tok, eq + 1);
        }
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/force_acid_vst.log", "a");
    {
        std::lock_guard<std::mutex> lk(g_api_init_lock);
        if (!g_api) {
            g_api = move_midi_fx_init(&g_host);
            if (!g_api || g_api->api_version != MIDI_FX_API_VERSION) { LOG("[acid_vst] core init failed\n"); g_api = nullptr; return nullptr; }
        }
    }
    Plugin *w = new Plugin();
    w->master = master;
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
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[acid_vst] up, %d params\n", NPARAMS);
    return e;
}
