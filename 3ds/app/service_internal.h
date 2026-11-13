#pragma once
// Shared declarations of the resident background process (the "service"). The process runs a handful of threads that
// communicate through the state below; each group is defined in the file named in its heading.
#include <3ds.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "protocol.h"
#include "frame.h"
#include "cast_client.h"
#include "capture_jpeg.h"
#include "video.h"
#include "game_audio.h"
#include "overlay.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

// ---- service_state.c: request/status state shared by every thread ---------------------------------------------
extern LightLock state_lock; // guards status, requested, requested_core, requested_source
extern NativeStatus status;
extern CastConfig requested;
extern atomic_bool wanted, quitting;
// The audio sampler must never wait for a lock (a preempted holder made it miss DSP ticks): it reads these atomics
// and only TRY-locks shared structures.
extern atomic_uint cfg_audio_enabled, cfg_audio_kbps, game_hi, game_lo;
static inline void publish_title(uint64_t title) {
    atomic_store(&game_lo, (unsigned)(title & 0xFFFFFFFFu));
    atomic_store(&game_hi, (unsigned)(title >> 32));
}
extern atomic_uint generation;        // bumped on every START/STOP request
extern atomic_uint active_generation; // the generation the stream worker is serving
extern Handle quit_event, server_port;
extern uint32_t requested_core, requested_source;
extern atomic_uint worker_token, chosen_token;
extern atomic_bool worker_started;
extern bool soc_ok, ssl_ok;
#define RAW_BUFFER_SIZE (400 * 4096) // one captured screen: widest width x padded column
#define SOC_BUFFER_SIZE 0x100000     // memory handed to socInit
extern void *soc_buffer;
extern atomic_uint stat_reconnects; // connection losses that led to a reconnect (shown on the TV)
bool cancelled(void *unused);
void set_status(unsigned state, const char *text);
void request_failed(unsigned expected, const char *message);
void stream_retry(unsigned expected, const char *message);

// ---- service_control.c: IPC with the frontend ---------------------------------------------------------------
void control_thread(void *unused);

// ---- service_audio.c: DSP sampler and packet queue ----------------------------------------------------------
typedef struct {
    unsigned kind, pushed, diag[7], hist[5], maxgap;
    NativeDspDebug dsp;
} AudioDebug;
extern LightLock audio_lock; // guards the packet queue and audio_dbg
extern AudioDebug audio_dbg;
extern atomic_uint dbg_probes, dbg_reads, dbg_silent;
extern atomic_uint audio_gen;
extern atomic_bool audio_started;
void audio_worker(void *argument);
bool audio_pop(AudioPacket *packet, unsigned epoch, uint64_t title);

// ---- service_overlay.c: in-game status overlay ---------------------------------------------------------------
void overlay_worker(void *unused);

// ---- service_encoder.c: capture + JPEG pipeline --------------------------------------------------------------
// Two slots give the pipeline a depth of two: the encoder fills one while the stream worker transmits the other.
#define SLOT_FREE 0
#define SLOT_READY 1
#define SLOT_SENDING 2
typedef struct {
    atomic_uint state;
    unsigned epoch;
    uint64_t seq;
    bool has[2];
    unsigned bytes; // Base64 bytes carried (for the throughput readout)
    char *img[2];   // Base64 text
    // Partial-update mode (the receiver composes RECT messages): img[i] then holds the Base64 text of nrect[i]
    // rectangles back to back (a whole screen is a single rectangle).
    bool rect_mode;
    unsigned nrect[2];
    NativeOutRect rect[2][NATIVE_MAX_RECTS];
} FrameSlot;
extern FrameSlot slots[2];
extern uint8_t *raw, *jpeg;
extern NativeVideoCache video[2];
extern char *b64[2];
// Set by the sender once the receiver said it composes rectangles; the encoder then produces partial updates.
extern atomic_bool rects_mode;
extern atomic_uint stat_rect_frames, stat_full_frames;
extern atomic_uint stat_q_min, stat_q_max, stat_q_now; // JPEG quality used in the last second (for the TV OSD)
extern LightLock enc_lock;                             // guards enc_cfg and enc_source
extern CastConfig enc_cfg;
extern unsigned enc_source;
extern atomic_bool enc_enabled;
extern atomic_uint enc_epoch;
extern atomic_uint stat_capture_ms, stat_encode_ms;
extern atomic_int encoder_core_request;
extern atomic_bool core2_failed;
FrameSlot *take_ready_slot(unsigned epoch);
Thread spawn_encoder(int core);

// ---- service_stream.c: the TLS sender (sole owner of the Cast client) ---------------------------------------
void stream_worker(void *argument);
