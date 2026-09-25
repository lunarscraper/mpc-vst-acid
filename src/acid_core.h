/* acid_core.h — minimal host-ABI shim for the ported Acid generator.
 *
 * schwung-acid's acid.c was written against two Schwung headers
 * (plugin_api_v1.h, midi_fx_api_v1.h). On the Force there is no Schwung host,
 * so this header re-declares ONLY the symbols acid_core.c actually references,
 * with the same names, values and layout. host_shim.cpp then plays the role of
 * the Schwung chain host: it fills a host_api_v1_t, calls move_midi_fx_init()
 * to get the vtable, and calls create_instance / process_midi / tick /
 * set_param / get_param / destroy_instance itself.
 *
 * If you ever need a symbol that Schwung's real headers have and this one
 * doesn't, add it here — do not pull in the full upstream headers, they drag
 * in the whole audio-plugin ABI.
 */
#ifndef ACID_CORE_H
#define ACID_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- from Schwung plugin_api_v1.h ------------------------------------------ */

/* acid.c does its 16th-note timing in "samples". We keep the same nominal
 * rate so samples_per_step math is byte-identical to the Move build; the
 * shim's timer feeds acid_tick() a matching frames/sample_rate pair. */
#define MOVE_SAMPLE_RATE 44100

#define MOVE_CLOCK_STATUS_UNAVAILABLE 0
#define MOVE_CLOCK_STATUS_STOPPED     1
#define MOVE_CLOCK_STATUS_RUNNING     2

/* Host callbacks. acid.c only ever uses these two, always guarded by a NULL
 * check, so the shim may leave either as NULL. */
typedef struct host_api_v1 {
    float (*get_bpm)(void);
    int   (*get_clock_status)(void);
} host_api_v1_t;

/* --- from Schwung midi_fx_api_v1.h --------------------------------------- */

#define MIDI_FX_API_VERSION 1

/* Same field order as upstream: acid.c initialises this struct with
 * designated initialisers, so only the names have to match. */
typedef struct midi_fx_api_v1 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    int   (*process_midi)(void *instance, const uint8_t *in_msg, int in_len,
                          uint8_t out_msgs[][3], int out_lens[], int max_out);
    int   (*tick)(void *instance, int frames, int sample_rate,
                  uint8_t out_msgs[][3], int out_lens[], int max_out);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;

/* Exported by acid_core.c; called once by the shim at startup. */
midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host);

/* Output-buffer sizing. Upstream midi_fx_api_v1.h defines this as 16; the
 * shim allocates its out arrays with this. */
#define MIDI_FX_MAX_OUT_MSGS 16

#ifdef __cplusplus
}
#endif

#endif /* ACID_CORE_H */
