#ifndef N3DS_CAST_CLIENT_H
#define N3DS_CAST_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include "config.h"
#include "audio_stream.h"

#define CAST_PORT 8009

typedef enum {
    CAST_STATE_DISCONNECTED = 0,
    CAST_STATE_CONNECTING,
    CAST_STATE_LAUNCHING_APP,
    CAST_STATE_STREAMING,
    CAST_STATE_ERROR
} CastClientState;

typedef struct {
    int sock_fd;
    CastClientState state;
    char status_msg[128];
    char transport_id[64];
    int request_id;
    uint64_t last_ping_ms;
    bool peer_rects; // receiver answered CAPS with "rects":true: it composes RECT messages (partial screen updates)
    // The PC receiver names a plain TCP port in its HELLO ("direct"): the pictures then go there, length-prefixed and
    // without TLS (which costs the 3DS more CPU than the network). The TLS connection stays for everything else, and is
    // the fallback if the direct one cannot be made or breaks.
    int direct_fd;
    unsigned direct_port;
    bool direct_failed;       // the direct connection broke or could not be made
    bool direct_was_ok;       // ... a picture went out over it at some point: then it is worth trying again
    uint64_t direct_retry_at; // not before this time (the last cast_client_poll time base), ms
    uint64_t poll_ms;         // the time of the last cast_client_poll
    // The receiver sent "NEED_KEY": it has no picture to build on (a page just opened), the next picture must be a
    // whole one. The caller clears it.
    bool key_request;
    // Set by the caller before cast_client_connect_and_launch: stop a receiver app that is already running on the
    // TV (e.g. sent to the background by the Back key) so the launch brings a fresh one to the front. Leave it
    // false for automatic reconnects so a short Wi-Fi drop re-joins the running app without reloading it.
    bool restart_app;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool tls_initialized;
    bool ssl_setup_done;
    // Optional cooperative cancellation for background connection attempts.
    bool (*cancelled)(void *context);
    void *cancel_context;

    uint8_t *tx_buf;
    size_t tx_buf_cap;
    uint8_t *rx_buf;
    size_t rx_buf_cap;
} CastClient;

bool cast_client_init(CastClient *client);
void cast_client_cleanup(CastClient *client);

/**
 * Automatically discover a Google TV / Chromecast on the LAN via mDNS (_googlecast._tcp.local).
 * Writes discovered IPv4 string to out_ip and returns true if found.
 */
bool cast_client_discover_tv_ip(char *out_ip, size_t ip_cap);

/**
 * Connect to Google TV at cfg->tv_ip:8009 over TLS and launch cfg->app_id.
 */
bool cast_client_connect_and_launch(CastClient *client, const CastConfig *cfg);

/**
 * Disconnect gracefully from Google TV.
 */
void cast_client_disconnect(CastClient *client);

/**
 * Poll incoming messages (e.g. PING -> PONG, RECEIVER_STATUS) and send periodic heartbeat.
 */
bool cast_client_poll(CastClient *client, uint64_t now_ms);

/**
 * Push updated projection config (layout, filter, stretch, OSD) to the TV web receiver.
 */
bool cast_client_send_config(CastClient *client, const CastConfig *cfg);
bool cast_client_send_audio(CastClient *client, const AudioPacket *packet);
bool cast_client_stop_audio(CastClient *client);
/** Send a small custom-namespace JSON message (debug statistics). */
bool cast_client_send_json(CastClient *client, const char *json);
/** Accumulated ARM11 ticks spent in send() (268111856 ticks per second). */
extern volatile uint64_t cast_net_ticks;
extern volatile uint64_t cast_write_ticks;
extern volatile uint64_t cast_build_ticks; // time spent building the messages (JSON text, Cast framing)

/**
 * Asks the receiver what it understands; it answers HELLO ("rects":true = partial screen updates). Tells it this
 * program's version and, when cfg->lang is set, the language to show its texts in.
 */
bool cast_client_send_caps(CastClient *client, const CastConfig *cfg);

/** One rectangle of a screen as its own small Base64 JPEG (partial updates). screen: 0 top, 1 bottom. */
typedef struct {
    uint8_t screen;
    uint16_t x, y, w, h;
    const char *b64;
    size_t len;
} CastRect;
/**
 * Sends rectangles as RECT messages (one rectangle per message, or numbered pieces of 48000 characters when a
 * rectangle is bigger). The first message carries the display config; "cont":1 marks every message of the frame
 * but the last, so the receiver counts one frame. Needs peer_rects.
 */
bool cast_client_send_rects(CastClient *client, const CastConfig *cfg, const CastRect *rects, unsigned count);
/** Negotiated TLS ciphersuite name ("" before the handshake). */
const char *cast_client_cipher(const CastClient *client);

/**
 * Send a compressed Base64 frame (top and/or bottom screen) over the custom Cast namespace.
 */
bool cast_client_send_frame(CastClient *client, const CastConfig *cfg, const char *top_b64, const char *bot_b64);

#endif // N3DS_CAST_CLIENT_H
