// Tests the real cast_client.c (and cast_proto.c, capture_jpeg.c) against a scripted receiver.
//
// Only the outside world is replaced: the socket calls that cast_client.c uses are renamed to the stub_* functions
// below by test.sh (-Dsocket=stub_socket ...), and mbedtls is replaced by a fake TLS pipe. The Cast messages the
// client writes are decoded with the real protocol code, and the receiver's answers are encoded with it, so what is
// tested is the client's behaviour and not how its source happens to be written.
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "cast_client.h"
#include "cast_proto.h"
#include "version.h"

#define FD 4
#define MAX_SCRIPT 64
#define MAX_SENT 64

// ---- the scripted receiver: what the client reads ----
typedef struct {
    uint8_t *frame; // length header + protobuf, as on the wire
    size_t size;
    unsigned after; // available once the client has polled for incoming data this many times
} Incoming;
static Incoming script[MAX_SCRIPT];
static unsigned script_count, script_head;
static size_t script_offset;
static unsigned in_polls;

static void receiver_says(const char *ns, const char *payload, unsigned after) {
    assert(script_count < MAX_SCRIPT);
    uint8_t *frame = malloc(1 << 17);
    size_t size =
        cast_proto_encode_utf8(frame, 1 << 17, CAST_DEFAULT_DEST, CAST_DEFAULT_SOURCE, ns, payload, strlen(payload));
    assert(size);
    script[script_count++] = (Incoming){frame, size, after};
}
static bool incoming_ready(void) {
    return script_head < script_count && script[script_head].after <= in_polls;
}

// ---- what the client wrote ----
typedef struct {
    char destination[64], ns[128];
    char *payload;
} Sent;
static Sent sent[MAX_SENT];
static unsigned sent_count;

static void record_written(const uint8_t *data, size_t len) {
    size_t at = 0;
    while (at + 4 <= len) {
        size_t body =
            ((size_t)data[at] << 24) | ((size_t)data[at + 1] << 16) | ((size_t)data[at + 2] << 8) | data[at + 3];
        assert(at + 4 + body <= len && sent_count < MAX_SENT);
        CastMessage message;
        assert(cast_proto_decode(data + at + 4, body, &message));
        Sent *entry = &sent[sent_count++];
        snprintf(entry->destination, sizeof(entry->destination), "%s", message.destination_id);
        snprintf(entry->ns, sizeof(entry->ns), "%s", message.ns);
        entry->payload = strdup(message.payload_utf8 ? message.payload_utf8 : "");
        assert(message.payload_len < 64000); // the Cast limit for one message is 64 KB
        cast_proto_free_message(&message);
        at += 4 + body;
    }
}
static unsigned count_sent(const char *needle) {
    unsigned n = 0;
    for (unsigned i = 0; i < sent_count; i++) {
        if (strstr(sent[i].payload, needle)) {
            n++;
        }
    }
    return n;
}

static int mode, polls, handshakes, closed;
static bool stop;

static void reset(void) {
    for (unsigned i = 0; i < script_count; i++) {
        free(script[i].frame);
    }
    for (unsigned i = 0; i < sent_count; i++) {
        free(sent[i].payload);
    }
    script_count = script_head = sent_count = 0;
    script_offset = 0;
    in_polls = 0;
    polls = handshakes = closed = 0;
    stop = false;
}

// ---- socket stubs (renamed in cast_client.c by -D) ----
int stub_socket(int family, int type, int protocol) {
    assert(family == AF_INET && type == SOCK_STREAM && !protocol);
    return FD;
}
int stub_fcntl(int fd, int command, ...) {
    assert(fd == FD);
    (void)command;
    return 0;
}
int stub_connect(int fd, const struct sockaddr *address, socklen_t size) {
    assert(fd == FD && address && size);
    errno = EINPROGRESS;
    return -1;
}
int stub_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    (void)fd;
    (void)level;
    (void)option;
    (void)value;
    (void)size;
    return 0;
}
int stub_close(int fd) {
    assert(fd == FD);
    closed++;
    return 0;
}
int stub_usleep(useconds_t usec) {
    (void)usec;
    return 0;
}
// glibc declares poll()'s first argument write-only, and the renamed declaration carries that over to the stub.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
int stub_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 1 && fds[0].fd == FD && timeout > 0);
    if (fds[0].events & POLLOUT) { // waiting for the TCP connection
        polls++;
        if (mode == 1) { // the connection never completes; the user gives up after the second wait
            if (polls == 2) {
                stop = true;
            }
            return 0;
        }
        fds[0].revents = POLLOUT;
        return 1;
    }
    in_polls++; // waiting for something from the receiver
    if (mode == 3) {
        stop = true;
        return 0;
    }
    if (incoming_ready()) {
        fds[0].revents = POLLIN;
        return 1;
    }
    return 0;
}
#pragma GCC diagnostic pop

// ---- fake TLS: the "encrypted" stream is the plain Cast stream ----
void mbedtls_ssl_init(mbedtls_ssl_context *ssl) {
    memset(ssl, 0, sizeof(*ssl));
}
void mbedtls_ssl_config_init(mbedtls_ssl_config *config) {
    memset(config, 0, sizeof(*config));
}
void mbedtls_entropy_init(mbedtls_entropy_context *entropy) {
    memset(entropy, 0, sizeof(*entropy));
}
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *drbg) {
    memset(drbg, 0, sizeof(*drbg));
}
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *drbg, int (*f_entropy)(void *, unsigned char *, size_t), void *p,
                          const unsigned char *custom, size_t len) {
    (void)drbg, (void)f_entropy, (void)p, (void)custom, (void)len;
    return 0;
}
int mbedtls_ctr_drbg_random(void *p, unsigned char *output, size_t len) {
    (void)p, (void)output, (void)len;
    return 0;
}
int mbedtls_entropy_func(void *data, unsigned char *output, size_t len) {
    (void)data, (void)output, (void)len;
    return 0;
}
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *drbg) {
    (void)drbg;
}
void mbedtls_entropy_free(mbedtls_entropy_context *entropy) {
    (void)entropy;
}
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *config, int endpoint, int transport, int preset) {
    (void)config, (void)endpoint, (void)transport, (void)preset;
    return 0;
}
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *config, int authmode) {
    (void)config, (void)authmode;
}
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *config, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng) {
    (void)config, (void)f_rng, (void)p_rng;
}
void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *config, const int *ciphersuites) {
    (void)config, (void)ciphersuites;
}
const int *mbedtls_ssl_list_ciphersuites(void) {
    return NULL;
}
void mbedtls_ssl_config_free(mbedtls_ssl_config *config) {
    (void)config;
}
int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *config) {
    (void)ssl, (void)config;
    return 0;
}
int mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl) {
    (void)ssl;
    return 0;
}
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *p_bio, mbedtls_ssl_send_t *f_send, mbedtls_ssl_recv_t *f_recv,
                         mbedtls_ssl_recv_timeout_t *f_recv_timeout) {
    (void)ssl, (void)p_bio, (void)f_send, (void)f_recv, (void)f_recv_timeout;
}
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
    (void)ssl;
    handshakes++;
    if (mode == 2) { // the user gives up during the handshake
        stop = true;
        return -1;
    }
    return 0;
}
int mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl) {
    (void)ssl;
    return 0;
}
void mbedtls_ssl_free(mbedtls_ssl_context *ssl) {
    (void)ssl;
}
const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *ssl) {
    (void)ssl;
    return "TEST";
}
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len) {
    (void)ssl;
    record_written(buf, len);
    return (int)len;
}
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len) {
    (void)ssl;
    if (!incoming_ready()) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    Incoming *message = &script[script_head];
    size_t n = message->size - script_offset < len ? message->size - script_offset : len;
    memcpy(buf, message->frame + script_offset, n);
    script_offset += n;
    if (script_offset == message->size) {
        script_head++;
        script_offset = 0;
    }
    return (int)n;
}

// ---- helpers ----
#define APP_ID "01B7A380"
static const char *RUNNING = "{\"type\":\"RECEIVER_STATUS\",\"status\":{\"applications\":[{\"appId\":\"" APP_ID
                             "\",\"sessionId\":\"S-1\",\"transportId\":\"web-1\"}]}}";
static const char *IDLE = "{\"type\":\"RECEIVER_STATUS\",\"status\":{\"volume\":{}}}";
static const char *LAUNCHED = "{\"type\":\"RECEIVER_STATUS\",\"status\":{\"applications\":[{\"appId\":\"" APP_ID
                              "\",\"transportId\":\"web-1\"}]}}";

static bool cancel_callback(void *context) {
    assert(context == &stop);
    return stop;
}
static CastConfig config(void) {
    CastConfig cfg = {0};
    strcpy(cfg.tv_ip, "192.168.0.1");
    strcpy(cfg.app_id, APP_ID);
    return cfg;
}
static CastClient new_client(void) {
    CastClient client = {0};
    assert(cast_client_init(&client));
    client.cancelled = cancel_callback;
    client.cancel_context = &stop;
    return client;
}
static char *make(size_t n, char c) {
    char *s = malloc(n + 1);
    memset(s, c, n);
    s[n] = 0;
    return s;
}

// ---- connect and launch ----
static void test_connect_and_cancel(void) {
    CastConfig cfg = config();
    for (mode = 0; mode <= 3; mode++) {
        reset();
        CastClient c = new_client();
        if (mode == 0) {
            receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
        }
        bool success = cast_client_connect_and_launch(&c, &cfg);
        if (mode == 0) {
            // CONNECT, LAUNCH, CONNECT to the app, CONFIG, CAPS
            assert(success && c.state == CAST_STATE_STREAMING && sent_count == 5 && !strcmp(c.transport_id, "web-1"));
            assert(count_sent("\"LAUNCH\"") == 1 && count_sent("\"CAPS\"") == 1);
            assert(count_sent("\"ver\":\"" NATIVE_APP_VERSION "\"") == 1); // the receiver is told which version this is
        } else {
            assert(!success && closed >= 1 && c.state != CAST_STATE_STREAMING);
        }
        if (mode == 1) {
            assert(polls == 2 && !handshakes && !sent_count);
        }
        if (mode == 2) {
            assert(handshakes == 1 && !sent_count);
        }
        if (mode == 3) {
            assert(in_polls == 1);
        }
        cast_client_cleanup(&c);
    }
    // The receiver is also told the language to speak; it is optional, and only a plain language tag is sent.
    const char *tags[] = {"", "ja", "x\"y", "zh-TW", NULL};
    const unsigned expected[] = {0, 1, 0, 1, 0};
    for (unsigned i = 0; i < sizeof(tags) / sizeof(*tags); i++) {
        mode = 0;
        reset();
        CastClient c = new_client();
        receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
        if (tags[i]) {
            snprintf(cfg.lang, sizeof(cfg.lang), "%s", tags[i]);
        } else {
            memset(cfg.lang, 'a', sizeof(cfg.lang)); // no terminator
        }
        assert(cast_client_connect_and_launch(&c, &cfg) && count_sent("\"CAPS\"") == 1);
        assert(count_sent("\"lang\":") == expected[i]);
        if (expected[i]) {
            char wanted[32]; // (tags[i] is not NULL here)
            snprintf(wanted, sizeof(wanted), "\"lang\":\"%s\"", tags[i]);
            assert(count_sent(wanted) == 1);
        }
        cast_client_cleanup(&c);
    }
    cfg.lang[0] = 0;
    // A client without a cancel callback still connects (old callers).
    mode = 0;
    reset();
    CastClient legacy = new_client();
    legacy.cancelled = NULL;
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
    assert(cast_client_connect_and_launch(&legacy, &cfg));
    cast_client_cleanup(&legacy);
    // An invalid address is reported and nothing is sent.
    reset();
    CastClient bad = new_client();
    CastConfig wrong = config();
    strcpy(wrong.tv_ip, "not-an-ip");
    assert(!cast_client_connect_and_launch(&bad, &wrong) && bad.state == CAST_STATE_ERROR && !sent_count);
    cast_client_cleanup(&bad);
}

// A fresh start restarts a running TV app; an automatic reconnect must not touch it.
static void test_restart(void) {
    CastConfig cfg = config();
    mode = 0;
    // running -> STOP with the session id -> status without the app -> LAUNCH
    reset();
    CastClient fresh = new_client();
    fresh.restart_app = true;
    receiver_says(CAST_NS_RECEIVER, RUNNING, 0);
    receiver_says(CAST_NS_RECEIVER, IDLE, 0);
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
    assert(cast_client_connect_and_launch(&fresh, &cfg));
    assert(count_sent("GET_STATUS") == 1 && count_sent("\"STOP\"") == 1 && count_sent("S-1") == 1);
    cast_client_cleanup(&fresh);
    // not running -> no STOP
    reset();
    CastClient idle = new_client();
    idle.restart_app = true;
    receiver_says(CAST_NS_RECEIVER, IDLE, 0);
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
    assert(cast_client_connect_and_launch(&idle, &cfg) && count_sent("GET_STATUS") == 1 && !count_sent("\"STOP\""));
    cast_client_cleanup(&idle);
    // TV silent for the status request -> no STOP, the launch still goes ahead
    reset();
    CastClient silent = new_client();
    silent.restart_app = true;
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 100);
    assert(cast_client_connect_and_launch(&silent, &cfg) && !count_sent("\"STOP\""));
    cast_client_cleanup(&silent);
    // STOP never confirmed (the status keeps listing the app): bounded, exactly one STOP, then the launch goes ahead
    reset();
    CastClient stuck = new_client();
    stuck.restart_app = true;
    for (int i = 0; i < 5; i++) {
        receiver_says(CAST_NS_RECEIVER, RUNNING, 0);
    }
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
    assert(cast_client_connect_and_launch(&stuck, &cfg) && count_sent("\"STOP\"") == 1);
    cast_client_cleanup(&stuck);
    // an automatic reconnect never asks the TV about a running app
    reset();
    CastClient again = new_client();
    receiver_says(CAST_NS_RECEIVER, LAUNCHED, 0);
    assert(cast_client_connect_and_launch(&again, &cfg) && !count_sent("GET_STATUS") && !count_sent("\"STOP\""));
    cast_client_cleanup(&again);
    // the TV refuses the app
    reset();
    CastClient refused = new_client();
    receiver_says(CAST_NS_RECEIVER, "{\"type\":\"LAUNCH_ERROR\",\"reason\":\"NOT_FOUND\"}", 0);
    assert(!cast_client_connect_and_launch(&refused, &cfg) && refused.state == CAST_STATE_ERROR);
    assert(strstr(refused.status_msg, "LAUNCH_ERROR"));
    cast_client_cleanup(&refused);
}

// ---- messages above the 64 KB Cast limit ----
static const char *field(const char *json, const char *key,
                         size_t *len) { // value of "key":"..." (Base64 has no escapes)
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return NULL;
    }
    p += strlen(pattern);
    *len = (size_t)(strchr(p, '"') - p);
    return p;
}
static unsigned number(const char *json, const char *key) {
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    assert(p);
    return (unsigned)atoi(p + strlen(pattern));
}

// Rebuilds both screens from what was sent and checks the framing rules.
static void check_frame(const char *top, const char *bottom, unsigned expected_messages) {
    char *rebuilt_top = calloc(1, 200000), *rebuilt_bottom = calloc(1, 200000);
    size_t top_len = 0, bottom_len = 0;
    for (unsigned i = 0; i < sent_count; i++) {
        const char *message = sent[i].payload;
        size_t len;
        assert((i == sent_count - 1) ==
               (strstr(message, "\"cont\":1") == NULL)); // every message but the last: more follows
        if (strstr(message, "\"FRAMEPART\"")) {
            const char *which = strstr(message, "\"w\":\"");
            const char *data = field(message, "d", &len);
            assert(which && data);
            if (!strncmp(which + 5, "top", 3)) {
                memcpy(rebuilt_top + top_len, data, len);
                top_len += len;
            } else {
                memcpy(rebuilt_bottom + bottom_len, data, len);
                bottom_len += len;
            }
        } else {
            const char *data = field(message, "top", &len);
            if (data) {
                memcpy(rebuilt_top + top_len, data, len);
                top_len += len;
            }
            data = field(message, "bot", &len);
            if (data) {
                memcpy(rebuilt_bottom + bottom_len, data, len);
                bottom_len += len;
            }
        }
    }
    assert(sent_count == expected_messages);
    assert(top_len == (top ? strlen(top) : 0) && bottom_len == (bottom ? strlen(bottom) : 0));
    assert(!top || !strcmp(rebuilt_top, top));
    assert(!bottom || !strcmp(rebuilt_bottom, bottom));
    free(rebuilt_top);
    free(rebuilt_bottom);
}

static CastClient streaming_client(void) {
    CastClient c = new_client();
    c.state = CAST_STATE_STREAMING;
    strcpy(c.transport_id, "web-1");
    return c;
}

static void test_frame_split(void) {
    CastClient c = streaming_client();
    CastConfig cfg = {0};
    char *t, *b;
    reset();
    t = make(20000, 'T');
    b = make(15000, 'B'); // fits in one message
    assert(cast_client_send_frame(&c, &cfg, t, b));
    check_frame(t, b, 1);
    reset();
    free(t);
    free(b);
    t = make(50000, 'T');
    b = make(30000, 'B'); // together too big: one message per screen
    assert(cast_client_send_frame(&c, &cfg, t, b));
    check_frame(t, b, 2);
    reset();
    free(t);
    free(b);
    t = make(128000, 'T');
    b = make(20000, 'B'); // three pieces for the top, then the bottom screen
    assert(cast_client_send_frame(&c, &cfg, t, b));
    check_frame(t, b, 4);
    reset();
    free(t);
    free(b);
    t = make(70000, 'T'); // top alone, two pieces
    assert(cast_client_send_frame(&c, &cfg, t, NULL));
    check_frame(t, NULL, 2);
    reset();
    free(t);
    b = make(56001, 'B'); // just over the single-message limit
    assert(cast_client_send_frame(&c, &cfg, NULL, b));
    check_frame(NULL, b, 2);
    reset();
    free(b);
    cast_client_cleanup(&c);
}

// ---- RECT messages for partial updates ----
static void test_rect_messages(void) {
    CastClient c = streaming_client();
    CastConfig cfg = {0};
    char *a = make(900, 'a'), *b = make(1200, 'b'), *big = make(100000, 'z');
    reset();
    // two small rectangles: one message each, cfg only in the first, cont on the first only
    CastRect two[2] = {{0, 48, 64, 16, 16, a, 900}, {1, 304, 224, 16, 16, b, 1200}};
    assert(cast_client_send_rects(&c, &cfg, two, 2) && sent_count == 2);
    assert(strstr(sent[0].payload, "\"type\":\"RECT\"") && strstr(sent[0].payload, "\"s\":\"top\"") &&
           number(sent[0].payload, "x") == 48 && number(sent[0].payload, "y") == 64 &&
           number(sent[0].payload, "w") == 16);
    assert(strstr(sent[0].payload, "\"cfg\":{") && strstr(sent[0].payload, "\"cont\":1") &&
           number(sent[0].payload, "i") == 0 && number(sent[0].payload, "n") == 1);
    assert(strstr(sent[1].payload, "\"s\":\"bot\"") && !strstr(sent[1].payload, "\"cfg\"") &&
           !strstr(sent[1].payload, "\"cont\"") && number(sent[1].payload, "x") == 304);
    assert(strstr(sent[0].payload, "\"d\":\"aaaa") && strstr(sent[1].payload, "bbbb\"}"));
    reset();
    // one 100000-character rectangle: three pieces of one rectangle (i 0..2 of 3), cont on all but the last
    CastRect one[1] = {{0, 0, 0, 400, 240, big, 100000}};
    assert(cast_client_send_rects(&c, &cfg, one, 1) && sent_count == 3);
    size_t got = 0;
    char *joined = malloc(100001);
    for (unsigned i = 0; i < 3; i++) {
        assert(number(sent[i].payload, "i") == i && number(sent[i].payload, "n") == 3 &&
               number(sent[i].payload, "w") == 400);
        assert((i < 2) == (strstr(sent[i].payload, "\"cont\":1") != NULL) &&
               (i == 0) == (strstr(sent[i].payload, "\"cfg\"") != NULL));
        const char *data = strstr(sent[i].payload, "\"d\":\"") + 5;
        size_t n = strlen(data) - 2; // minus the closing "}
        memcpy(joined + got, data, n);
        got += n;
    }
    joined[got] = 0;
    assert(got == 100000 && !strcmp(joined, big));
    reset();
    // rectangles mixed with a big one: cont flags follow the MESSAGE count (1 + 3 + 1 = 5)
    CastRect mix[3] = {{0, 16, 16, 16, 16, a, 900}, {0, 0, 0, 400, 240, big, 100000}, {1, 0, 0, 16, 16, b, 1200}};
    assert(cast_client_send_rects(&c, &cfg, mix, 3) && sent_count == 5);
    for (unsigned i = 0; i < 5; i++) {
        assert((i < 4) == (strstr(sent[i].payload, "\"cont\":1") != NULL));
    }
    reset();
    // refusals
    CastRect bad[1] = {{0, 0, 0, 16, 16, NULL, 0}};
    assert(!cast_client_send_rects(&c, &cfg, bad, 1) && !cast_client_send_rects(&c, &cfg, two, 0) && !sent_count);
    c.state = CAST_STATE_DISCONNECTED;
    assert(!cast_client_send_rects(&c, &cfg, two, 2));
    free(a);
    free(b);
    free(big);
    free(joined);
    cast_client_cleanup(&c);
}

int main(void) {
    test_connect_and_cancel();
    test_restart();
    test_frame_split();
    test_rect_messages();
    reset();
    puts("PASS: real Cast client against a scripted receiver: connect, cancel at every wait, restart of a running app, "
         "LAUNCH_ERROR, <64 KB framing with numbered pieces, RECT messages");
    return 0;
}
