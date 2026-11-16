#include "cast_client.h"
#include "cast_proto.h"
#include "version.h"
#include "capture_jpeg.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#ifdef __3DS__
#include <3ds.h>
#endif

// Time spent inside send() (network/SOC service), for splitting T into network vs CPU.
volatile uint64_t cast_net_ticks = 0;
// Time spent inside mbedtls_ssl_write (includes send()): write - net = encryption and copies.
volatile uint64_t cast_write_ticks = 0;
// Time spent building the messages (JSON text with the picture, the Cast framing): not the network, not encryption.
volatile uint64_t cast_build_ticks = 0;

#define TX_BUF_SIZE (128 * 1024)
#define RX_BUF_SIZE (32 * 1024)
#define CONNECT_POLL_STEPS 40 // the TCP connect gets 40 x 100 ms
#define CONNECT_POLL_MS 100
#define DIRECT_RETRY_MS 5000         // a direct connection that broke is tried again after this long
#define SEND_BUFFER_SIZE (64 * 1024) // fit a whole JPEG frame per burst
#define HANDSHAKE_RETRIES 400        // the TLS handshake gets 400 x 5 ms
#define HANDSHAKE_SLEEP_US 5000
#define LAUNCH_WAIT_STEPS 100 // the receiver gets 100 x 100 ms to report that the app is running
#define MESSAGE_WAIT_MS 100
#define STOP_WAIT_STEPS 15 // restarting: wait this many x 100 ms for the first status
#define STOP_CONFIRM_WAIT_STEPS 10
#define STOP_CONFIRM_TRIES 4
#define STOP_SETTLE_US 300000 // short pause so the TV finishes tearing the page down

static bool client_cancelled(const CastClient *client) {
    return client->cancelled && client->cancelled(client->cancel_context);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
#ifdef __3DS__
    uint64_t net_start = svcGetSystemTick();
#endif
    ssize_t ret = send(fd, buf, len, 0);
#ifdef __3DS__
    cast_net_ticks += svcGetSystemTick() - net_start;
#endif
    if (ret < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)ret;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

    ssize_t ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    return (int)ret;
}

bool cast_client_init(CastClient *client) {
    memset(client, 0, sizeof(CastClient));
    client->sock_fd = -1;
    client->direct_fd = -1;
    client->state = CAST_STATE_DISCONNECTED;
    snprintf(client->status_msg, sizeof(client->status_msg), "Idle (Ready)");

    client->tx_buf_cap = TX_BUF_SIZE;
    client->tx_buf = (uint8_t *)malloc(client->tx_buf_cap);
    client->rx_buf_cap = RX_BUF_SIZE;
    client->rx_buf = (uint8_t *)malloc(client->rx_buf_cap);

    if (!client->tx_buf || !client->rx_buf) {
        cast_client_cleanup(client);
        return false;
    }

    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->conf);
    mbedtls_entropy_init(&client->entropy);
    mbedtls_ctr_drbg_init(&client->ctr_drbg);
    client->tls_initialized = true;

    const char *pers = "n3ds_cast_client";
    if (mbedtls_ctr_drbg_seed(&client->ctr_drbg, mbedtls_entropy_func, &client->entropy, (const unsigned char *)pers,
                              strlen(pers)) != 0) {
        snprintf(client->status_msg, sizeof(client->status_msg), "RNG seed failed");
        return false;
    }

    if (mbedtls_ssl_config_defaults(&client->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(client->status_msg, sizeof(client->status_msg), "SSL config failed");
        return false;
    }

    // Prefer ChaCha20-Poly1305: on the ARM11 (no AES instructions) it is much cheaper than
    // software AES-GCM. The default list follows, so a TV without ChaCha still connects.
    {
        static int suites[64];
        int n = 0;
#if defined(MBEDTLS_CHACHAPOLY_C)
        suites[n++] = MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256;
        suites[n++] = MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256;
#endif
        const int *defaults = mbedtls_ssl_list_ciphersuites();
        for (int i = 0; defaults && defaults[i] && n < 62; i++) {
            bool dup = false;
            for (int j = 0; j < n; j++) {
                if (suites[j] == defaults[i]) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                suites[n++] = defaults[i];
            }
        }
        suites[n] = 0;
        mbedtls_ssl_conf_ciphersuites(&client->conf, suites);
    }

    // Cast devices use self-signed device certificates on port 8009
    mbedtls_ssl_conf_authmode(&client->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&client->conf, mbedtls_ctr_drbg_random, &client->ctr_drbg);

    if (mbedtls_ssl_setup(&client->ssl, &client->conf) != 0) {
        snprintf(client->status_msg, sizeof(client->status_msg), "SSL setup failed");
        return false;
    }
    mbedtls_ssl_set_bio(&client->ssl, &client->sock_fd, bio_send, bio_recv, NULL);
    client->ssl_setup_done = true;

    return true;
}

static void direct_close(CastClient *client) {
    if (client->direct_fd >= 0) {
        close(client->direct_fd);
        client->direct_fd = -1;
    }
}

void cast_client_disconnect(CastClient *client) {
    direct_close(client);
    client->direct_port = 0;
    client->direct_failed = false;
    client->direct_was_ok = false;
    client->direct_retry_at = 0;
    client->key_request = false;
    if (client->sock_fd >= 0) {
        if (client->ssl_setup_done) {
            mbedtls_ssl_close_notify(&client->ssl);
        }
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    if (client->ssl_setup_done) {
        mbedtls_ssl_session_reset(&client->ssl);
    }
    client->transport_id[0] = '\0';
    client->peer_rects = false;
    if (client->state != CAST_STATE_ERROR) {
        client->state = CAST_STATE_DISCONNECTED;
        snprintf(client->status_msg, sizeof(client->status_msg), "Disconnected");
    }
}

void cast_client_cleanup(CastClient *client) {
    cast_client_disconnect(client);
    if (client->tls_initialized) {
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_config_free(&client->conf);
        mbedtls_ctr_drbg_free(&client->ctr_drbg);
        mbedtls_entropy_free(&client->entropy);
        client->tls_initialized = false;
        client->ssl_setup_done = false;
    }
    if (client->tx_buf) {
        free(client->tx_buf);
        client->tx_buf = NULL;
    }
    if (client->rx_buf) {
        free(client->rx_buf);
        client->rx_buf = NULL;
    }
}

bool cast_client_discover_tv_ip(char *out_ip, size_t ip_cap) {
    // mDNS query packet for PTR _googlecast._tcp.local (Unicast response requested)
    static const uint8_t mdns_query[] = {
        0x00, 0x00, // Transaction ID
        0x00, 0x00, // Flags: standard query
        0x00, 0x01, // Questions: 1
        0x00, 0x00, // Answer RRs
        0x00, 0x00, // Authority RRs
        0x00, 0x00, // Additional RRs
        0x0b, '_',  'g', 'o', 'o',  'g', 'l', 'e', 'c', 'a', 's',  't',  0x04,
        '_',  't',  'c', 'p', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00, 0x00, 0x0c, // Type: PTR (12)
        0x80, 0x01 // Class: IN + QU (Unicast response preferred)
    };

    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) {
        return false;
    }

    int flags = fcntl(ufd, F_GETFL, 0);
    fcntl(ufd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in mcast;
    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mcast.sin_addr);

    // Send 2 queries to ensure delivery over Wi-Fi
    sendto(ufd, mdns_query, sizeof(mdns_query), 0, (struct sockaddr *)&mcast, sizeof(mcast));
    usleep(50000);
    sendto(ufd, mdns_query, sizeof(mdns_query), 0, (struct sockaddr *)&mcast, sizeof(mcast));

    struct pollfd pfd = {ufd, POLLIN, 0};
    bool found = false;
    for (int i = 0; i < 15; i++) {
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            uint8_t rbuf[512];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(ufd, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from_addr, &from_len);
            if (n > 12) {
                if (inet_ntop(AF_INET, &from_addr.sin_addr, out_ip, (socklen_t)ip_cap) != NULL) {
                    found = true;
                    break;
                }
            }
        }
    }

    close(ufd);
    return found;
}

static bool tls_write_all(CastClient *client, const uint8_t *data, size_t len) {
    size_t sent = 0;
    int retries = 0;
    while (sent < len) {
        if (client_cancelled(client)) {
            return false;
        }
#ifdef __3DS__
        uint64_t write_start = svcGetSystemTick();
#endif
        int r = mbedtls_ssl_write(&client->ssl, data + sent, len - sent);
#ifdef __3DS__
        cast_write_ticks += svcGetSystemTick() - write_start;
#endif
        if (r > 0) {
            sent += (size_t)r;
            retries = 0;
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
            // Wake as soon as the socket can move data instead of sleeping a
            // fixed 2 ms per retry (a small send buffer made that dominate T).
            // A Wi-Fi hiccup or a slow TV can stop the link for a second or two. TCP recovers by itself, but giving up
            // here costs a reconnect (backoff + TLS handshake: several seconds of frozen picture and no sound), so
            // only ~5 s without any progress counts as a dead link (each poll below waits up to 20 ms).
            if (++retries > 250) {
                return false;
            }
            struct pollfd pfd;
            pfd.fd = client->sock_fd;
            pfd.events = (r == MBEDTLS_ERR_SSL_WANT_WRITE) ? POLLOUT : POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, 20);
        } else {
            return false;
        }
    }
    return true;
}

static bool send_cast_packet(CastClient *client, const char *dest_id, const char *ns, const char *json_payload) {
#ifdef __3DS__
    uint64_t build_start = svcGetSystemTick();
#endif
    size_t pkt_len = cast_proto_encode_utf8(client->tx_buf, client->tx_buf_cap, CAST_DEFAULT_SOURCE, dest_id, ns,
                                            json_payload, strlen(json_payload));
#ifdef __3DS__
    cast_build_ticks += svcGetSystemTick() - build_start;
#endif
    if (pkt_len == 0) {
        return false;
    }
    return tls_write_all(client, client->tx_buf, pkt_len);
}

static bool extract_json_string(const char *json, const char *key, char *out_val, size_t out_cap) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':')) {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        out_val[i++] = *p++;
    }
    out_val[i] = '\0';
    return i > 0;
}

static bool read_one_cast_message(CastClient *client, CastMessage *out_msg, int timeout_ms) {
    if (client_cancelled(client)) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd = client->sock_fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }

    uint8_t hdr[4];
    size_t got = 0;
    int retries = 0;
    while (got < 4) {
        if (client_cancelled(client)) {
            return false;
        }
        int r = mbedtls_ssl_read(&client->ssl, hdr + got, 4 - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (got == 0) {
                return false;
            }
            if (++retries > 100) {
                return false;
            }
            usleep(2000);
        } else {
            return false;
        }
    }

    uint32_t msg_len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];

    if (msg_len == 0 || msg_len > client->rx_buf_cap) {
        return false;
    }

    got = 0;
    retries = 0;
    while (got < msg_len) {
        if (client_cancelled(client)) {
            return false;
        }
        int r = mbedtls_ssl_read(&client->ssl, client->rx_buf + got, msg_len - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++retries > 200) {
                return false;
            }
            usleep(2000);
        } else {
            return false;
        }
    }

    return cast_proto_decode(client->rx_buf, msg_len, out_msg);
}

// Waits up to wait_steps*100 ms for a RECEIVER_STATUS and reports whether it lists app_id (and its sessionId).
// Answers heartbeats meanwhile. Returns false if no status arrived in time or the client was cancelled.
static bool wait_receiver_status(CastClient *client, const char *app_id, int wait_steps, bool *app_running,
                                 char *session, size_t session_cap) {
    for (int step = 0; step < wait_steps; step++) {
        if (client_cancelled(client)) {
            return false;
        }
        CastMessage msg;
        if (!read_one_cast_message(client, &msg, MESSAGE_WAIT_MS)) {
            continue;
        }
        bool got = false;
        if (strcmp(msg.ns, CAST_NS_HEARTBEAT) == 0 && msg.payload_utf8 && strstr(msg.payload_utf8, "\"PING\"")) {
            send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_HEARTBEAT, "{\"type\":\"PONG\"}");
        } else if (strcmp(msg.ns, CAST_NS_RECEIVER) == 0 && msg.payload_utf8 &&
                   strstr(msg.payload_utf8, "RECEIVER_STATUS")) {
            *app_running = strstr(msg.payload_utf8, app_id) != NULL;
            session[0] = '\0';
            if (*app_running) {
                extract_json_string(msg.payload_utf8, "sessionId", session, session_cap);
            }
            got = true;
        }
        cast_proto_free_message(&msg);
        if (got) {
            return true;
        }
    }
    return false;
}

// If the receiver app is already running on the TV, ask it to stop and wait until the TV reports it gone.
// Returns true when nothing is running afterwards (or nothing was running). Best effort: on any
// uncertainty the caller still sends LAUNCH, exactly as before.
static bool stop_running_app(CastClient *client, const char *app_id) {
    char json[96], session[80];
    bool running = false;
    client->request_id++;
    snprintf(json, sizeof(json), "{\"type\":\"GET_STATUS\",\"requestId\":%d}", client->request_id);
    if (!send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_RECEIVER, json)) {
        return false;
    }
    if (!wait_receiver_status(client, app_id, STOP_WAIT_STEPS, &running, session, sizeof(session))) {
        return false;
    }
    if (!running) {
        return true;
    }
    if (!session[0]) {
        return false; // running but no session id to stop: fall back to plain LAUNCH
    }
    snprintf(client->status_msg, sizeof(client->status_msg), "Restarting receiver app...");
    client->request_id++;
    char stop[192];
    snprintf(stop, sizeof(stop), "{\"type\":\"STOP\",\"sessionId\":\"%s\",\"requestId\":%d}", session,
             client->request_id);
    if (!send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_RECEIVER, stop)) {
        return false;
    }
    // The TV answers with a RECEIVER_STATUS without our app once it has been closed.
    for (int tries = 0; tries < STOP_CONFIRM_TRIES; tries++) {
        bool still = true;
        char again[80];
        if (!wait_receiver_status(client, app_id, STOP_CONFIRM_WAIT_STEPS, &still, again, sizeof(again))) {
            if (client_cancelled(client)) {
                return false;
            }
            continue;
        }
        if (!still) {
            usleep(STOP_SETTLE_US);
            return true;
        } // short pause so the TV finishes tearing the page down
    }
    return false;
}

// Records an error, closes the connection and returns false: `return fail(client, "...")`.
static bool fail(CastClient *client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static bool fail(CastClient *client, const char *format, ...) {
    client->state = CAST_STATE_ERROR;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(client->status_msg, sizeof(client->status_msg), format, arguments);
    va_end(arguments);
    cast_client_disconnect(client);
    return false;
}

// Opens the TCP connection to the TV (non-blocking connect with a timeout).
static bool open_connection(CastClient *client, const CastConfig *cfg) {
    client->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sock_fd < 0) {
        client->state = CAST_STATE_ERROR;
        snprintf(client->status_msg, sizeof(client->status_msg), "Socket creation failed");
        return false;
    }
    int flags = fcntl(client->sock_fd, F_GETFL, 0);
    fcntl(client->sock_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CAST_PORT);
    if (inet_pton(AF_INET, cfg->tv_ip, &addr.sin_addr) <= 0) {
        return fail(client, "Invalid IP: %s", cfg->tv_ip);
    }
    int res = connect(client->sock_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        return fail(client, "Connect error (%d)", errno);
    }

    struct pollfd pfd = {client->sock_fd, POLLOUT, 0};
    bool connected = false;
    for (int step = 0; step < CONNECT_POLL_STEPS && !client_cancelled(client); step++) {
        int ready = poll(&pfd, 1, CONNECT_POLL_MS);
        if (ready > 0) {
            connected = true;
            break;
        }
        if (ready < 0 && errno != EINTR) {
            break;
        }
    }
    if (!connected || client_cancelled(client)) {
        return fail(client, "Connect timeout (%s)", cfg->tv_ip);
    }

    int one = 1;
    setsockopt(client->sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = SEND_BUFFER_SIZE; // best effort
    setsockopt(client->sock_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return true;
}

static bool tls_handshake(CastClient *client) {
    mbedtls_ssl_session_reset(&client->ssl);
    mbedtls_ssl_set_bio(&client->ssl, &client->sock_fd, bio_send, bio_recv, NULL);
    int retries = 0;
    int ret;
    while ((ret = mbedtls_ssl_handshake(&client->ssl)) != 0) {
        if (client_cancelled(client)) {
            cast_client_disconnect(client);
            return false;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return fail(client, "TLS handshake err: -0x%04X", -ret);
        }
        if (++retries > HANDSHAKE_RETRIES) {
            return fail(client, "TLS handshake timeout");
        }
        usleep(HANDSHAKE_SLEEP_US);
    }
    return true;
}

// Sends CONNECT to receiver-0 and then LAUNCH for the receiver app.
static bool request_launch(CastClient *client, const CastConfig *cfg) {
    client->state = CAST_STATE_LAUNCHING_APP;
    snprintf(client->status_msg, sizeof(client->status_msg), "Launching App ID %s...", cfg->app_id);
    if (!send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_CONNECTION, "{\"type\":\"CONNECT\"}")) {
        return fail(client, "Failed to send CONNECT");
    }
    // If the receiver is still running (e.g. the user pressed Back on the TV: the page lives on in the
    // background with only its audio), a plain LAUNCH just reports it as running and never brings it to the
    // front. Stop that session first so the LAUNCH below starts a fresh, visible one.
    if (client->restart_app && !stop_running_app(client, cfg->app_id)) {
        if (client->state == CAST_STATE_ERROR || client_cancelled(client)) {
            cast_client_disconnect(client);
            return false;
        }
    }
    char launch_json[160];
    client->request_id++;
    snprintf(launch_json, sizeof(launch_json), "{\"type\":\"LAUNCH\",\"appId\":\"%s\",\"requestId\":%d}", cfg->app_id,
             client->request_id);
    if (!send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_RECEIVER, launch_json)) {
        return fail(client, "Failed to send LAUNCH");
    }
    return true;
}

// Waits for the RECEIVER_STATUS that carries the transportId of the launched app.
static bool wait_for_launch(CastClient *client, const CastConfig *cfg) {
    for (int step = 0; step < LAUNCH_WAIT_STEPS; step++) {
        if (client_cancelled(client)) {
            cast_client_disconnect(client);
            return false;
        }
        CastMessage msg;
        if (!read_one_cast_message(client, &msg, MESSAGE_WAIT_MS)) {
            continue;
        }
        bool launched = false;
        if (strcmp(msg.ns, CAST_NS_HEARTBEAT) == 0 && msg.payload_utf8 && strstr(msg.payload_utf8, "\"PING\"")) {
            send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_HEARTBEAT, "{\"type\":\"PONG\"}");
        } else if (strcmp(msg.ns, CAST_NS_RECEIVER) == 0 && msg.payload_utf8) {
            if (strstr(msg.payload_utf8, cfg->app_id)) {
                launched = extract_json_string(msg.payload_utf8, "transportId", client->transport_id,
                                               sizeof(client->transport_id));
            } else if (strstr(msg.payload_utf8, "LAUNCH_ERROR")) {
                cast_proto_free_message(&msg);
                return fail(client, "LAUNCH_ERROR (Check App ID %s)", cfg->app_id);
            }
        }
        cast_proto_free_message(&msg);
        if (launched) {
            return true;
        }
    }
    return fail(client, "Launch timeout (App %s)", cfg->app_id);
}

bool cast_client_connect_and_launch(CastClient *client, const CastConfig *cfg) {
    if (!client->ssl_setup_done) {
        client->state = CAST_STATE_ERROR;
        snprintf(client->status_msg, sizeof(client->status_msg), "TLS not initialized");
        return false;
    }
    cast_client_disconnect(client);
    client->state = CAST_STATE_CONNECTING;
    snprintf(client->status_msg, sizeof(client->status_msg), "Connecting to %s:%d...", cfg->tv_ip, CAST_PORT);
    if (!open_connection(client, cfg) || !tls_handshake(client) || !request_launch(client, cfg) ||
        !wait_for_launch(client, cfg)) {
        return false;
    }
    // Connect to the launched web receiver's transportId
    if (!send_cast_packet(client, client->transport_id, CAST_NS_CONNECTION, "{\"type\":\"CONNECT\"}")) {
        return fail(client, "Transport connect failed");
    }
    client->state = CAST_STATE_STREAMING;
    client->last_ping_ms = 0;
    snprintf(client->status_msg, sizeof(client->status_msg), "Streaming to %s", cfg->tv_ip);
    cast_client_send_config(client, cfg); // the initial projection config
    client->peer_rects = false;
    cast_client_send_caps(client, cfg); // an up-to-date receiver answers HELLO and says what it understands
    return true;
}

bool cast_client_poll(CastClient *client, uint64_t now_ms) {
    if (client->state != CAST_STATE_STREAMING || client->sock_fd < 0) {
        return false;
    }
    client->poll_ms = now_ms;

    CastMessage msg;
    while (read_one_cast_message(client, &msg, 0)) {
        if (strcmp(msg.ns, CAST_NS_HEARTBEAT) == 0 && msg.payload_utf8) {
            if (strstr(msg.payload_utf8, "\"PING\"")) {
                send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_HEARTBEAT, "{\"type\":\"PONG\"}");
            }
        } else if (strcmp(msg.ns, CAST_NS_CONNECTION) == 0 && msg.payload_utf8) {
            if (strstr(msg.payload_utf8, "\"CLOSE\"")) {
                cast_proto_free_message(&msg);
                client->state = CAST_STATE_DISCONNECTED;
                snprintf(client->status_msg, sizeof(client->status_msg), "Closed by TV");
                cast_client_disconnect(client);
                return false;
            }
        } else if (strcmp(msg.ns, CAST_NS_CUSTOM) == 0 && msg.payload_utf8 && strstr(msg.payload_utf8, "\"HELLO\"") &&
                   strstr(msg.payload_utf8, "\"ok\":true")) {
            if (strstr(msg.payload_utf8, "\"rects\":true")) {
                client->peer_rects = true;
            }
            const char *direct = strstr(msg.payload_utf8, "\"direct\":");
            if (direct) {
                long port = strtol(direct + 9, NULL, 10);
                client->direct_port = port > 0 && port < 65536 ? (unsigned)port : 0;
            }
        } else if (strcmp(msg.ns, CAST_NS_CUSTOM) == 0 && msg.payload_utf8 &&
                   strstr(msg.payload_utf8, "\"NEED_KEY\"")) {
            client->key_request = true; // the page has no picture to build on (it was just opened)
        }
        cast_proto_free_message(&msg);
    }

    if (now_ms - client->last_ping_ms >= 5000) {
        client->last_ping_ms = now_ms;
        if (!send_cast_packet(client, CAST_DEFAULT_DEST, CAST_NS_HEARTBEAT, "{\"type\":\"PING\"}")) {
            client->state = CAST_STATE_ERROR;
            snprintf(client->status_msg, sizeof(client->status_msg), "Connection lost");
            cast_client_disconnect(client);
            return false;
        }
    }

    return true;
}

bool cast_client_send_config(CastClient *client, const CastConfig *cfg) {
    if (client->state != CAST_STATE_STREAMING || client->transport_id[0] == '\0') {
        return false;
    }
    char json[256];
    snprintf(
        json, sizeof(json),
        "{\"type\":\"CONFIG\",\"layout\":%d,\"filter\":%d,\"stretch\":%d,\"show_osd\":%d,\"audio_enabled\":%d,\"audio_kbps\":%d}",
        cfg->layout_mode, cfg->scale_filter, cfg->stretch_mode, cfg->show_osd, cfg->audio_enabled, cfg->audio_kbps);
    return send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
}

bool cast_client_send_audio(CastClient *client, const AudioPacket *packet) {
    if (client->state != CAST_STATE_STREAMING || !client->transport_id[0] || !packet || !packet->size ||
        packet->size > AUDIO_MAX_BYTES) {
        return false;
    }
    char b64[4 * ((AUDIO_MAX_BYTES + 2) / 3) + 1];
    char json[sizeof(b64) + 192];
    if (!capture_base64_encode(packet->data, packet->size, b64, sizeof(b64))) {
        return false;
    }
    int n =
        snprintf(json, sizeof(json),
                 "{\"type\":\"AUDIO\",\"codec\":\"ima-adpcm\",\"seq\":%lu,\"rate\":%d,\"samples\":%d,\"data\":\"%s\"}",
                 (unsigned long)packet->sequence, packet->sample_rate, packet->samples, b64);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return false;
    }
    return send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
}

bool cast_client_stop_audio(CastClient *client) {
    if (client->state != CAST_STATE_STREAMING || !client->transport_id[0]) {
        return false;
    }
    return send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, "{\"type\":\"AUDIO_STOP\"}");
}

// A Cast transport message is limited to 64 KB. A frame whose Base64 text does not fit in one message is
// sent as several: one "FRAME" per screen, and a screen that is still too big as numbered "FRAMEPART" pieces
// the receiver glues back together. "cont":1 marks every message of a frame except the last, so the receiver
// counts one frame (not several) for its FPS.
#define CAST_MSG_IMG_MAX 56000u // Base64 characters of images allowed in a single FRAME message
#define CAST_PART_CHARS 48000u  // Base64 characters per FRAMEPART

static void cfg_json(char *out, size_t cap, const CastConfig *cfg) {
    snprintf(
        out, cap, "{\"layout\":%d,\"filter\":%d,\"stretch\":%d,\"show_osd\":%d,\"audio_enabled\":%d,\"audio_kbps\":%d}",
        cfg->layout_mode, cfg->scale_filter, cfg->stretch_mode, cfg->show_osd, cfg->audio_enabled, cfg->audio_kbps);
}

// Sends one screen's Base64 JPEG, as one FRAME message or as FRAMEPART pieces. cont: more messages follow.
static bool send_one_screen(CastClient *client, const CastConfig *cfg, const char *which, const char *b64, size_t len,
                            bool cont) {
    char cj[160];
    cfg_json(cj, sizeof(cj), cfg);
    size_t parts = (len + CAST_PART_CHARS - 1) / CAST_PART_CHARS;
    if (len <= CAST_MSG_IMG_MAX) {
        parts = 1;
    }
    char *json = (char *)malloc((len <= CAST_MSG_IMG_MAX ? len : CAST_PART_CHARS) + 400);
    if (!json) {
        return false;
    }
    bool ok = true;
    if (len <= CAST_MSG_IMG_MAX) {
        snprintf(json, len + 400, "{\"type\":\"FRAME\",\"cfg\":%s%s,\"%s\":\"%s\"}", cj, cont ? ",\"cont\":1" : "",
                 which, b64);
        ok = send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
    } else {
        for (size_t i = 0; i < parts && ok; i++) {
            size_t off = i * CAST_PART_CHARS, n = len - off;
            if (n > CAST_PART_CHARS) {
                n = CAST_PART_CHARS;
            }
            bool last = (i + 1 == parts);
            int h = snprintf(json, 400, "{\"type\":\"FRAMEPART\",\"w\":\"%s\",\"i\":%u,\"n\":%u,\"cfg\":%s%s,\"d\":\"",
                             which, (unsigned)i, (unsigned)parts, cj, (cont || !last) ? ",\"cont\":1" : "");
            memcpy(json + h, b64 + off, n);
            memcpy(json + h + n, "\"}", 3);
            ok = send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
        }
    }
    free(json);
    return ok;
}

bool cast_client_send_frame(CastClient *client, const CastConfig *cfg, const char *top_b64, const char *bot_b64) {
    if (client->state != CAST_STATE_STREAMING || client->transport_id[0] == '\0') {
        return false;
    }

    size_t top_len = top_b64 ? strlen(top_b64) : 0;
    size_t bot_len = bot_b64 ? strlen(bot_b64) : 0;
    if (top_len + bot_len > CAST_MSG_IMG_MAX) {
        bool ok = true;
        if (top_len) {
            ok = send_one_screen(client, cfg, "top", top_b64, top_len, bot_len > 0);
        }
        if (ok && bot_len) {
            ok = send_one_screen(client, cfg, "bot", bot_b64, bot_len, false);
        }
        return ok;
    }
    size_t json_cap = top_len + bot_len + 256;
    char *json = (char *)malloc(json_cap);
    if (!json) {
        return false;
    }

    int n = snprintf(
        json, json_cap,
        "{\"type\":\"FRAME\",\"cfg\":{\"layout\":%d,\"filter\":%d,\"stretch\":%d,\"show_osd\":%d,\"audio_enabled\":%d,\"audio_kbps\":%d}",
        cfg->layout_mode, cfg->scale_filter, cfg->stretch_mode, cfg->show_osd, cfg->audio_enabled, cfg->audio_kbps);
    size_t pos = (size_t)n;

    if (top_b64 && top_len > 0) {
        pos += (size_t)snprintf(json + pos, json_cap - pos, ",\"top\":\"%s\"", top_b64);
    }
    if (bot_b64 && bot_len > 0) {
        pos += (size_t)snprintf(json + pos, json_cap - pos, ",\"bot\":\"%s\"", bot_b64);
    }
    pos += (size_t)snprintf(json + pos, json_cap - pos, "}");

    bool ok = send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
    free(json);
    return ok;
}

bool cast_client_send_json(CastClient *client, const char *json) {
    if (client->state != CAST_STATE_STREAMING || client->transport_id[0] == '\0' || !json) {
        return false;
    }
    return send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
}

const char *cast_client_cipher(const CastClient *client) {
    if (!client || !client->ssl_setup_done) {
        return "";
    }
    const char *name = mbedtls_ssl_get_ciphersuite(&client->ssl);
    return name ? name : "";
}

bool cast_client_send_caps(CastClient *client, const CastConfig *cfg) {
    if (client->state != CAST_STATE_STREAMING || client->transport_id[0] == '\0') {
        return false;
    }
    // A language tag is letters and hyphens only; anything else is not sent (it goes into JSON as it is).
    char lang[sizeof(cfg->lang) + 12] = "";
    size_t length = strnlen(cfg->lang, sizeof(cfg->lang));
    bool valid = length > 0 && length < sizeof(cfg->lang);
    for (size_t i = 0; valid && i < length; i++) {
        char c = cfg->lang[i];
        valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    }
    if (valid) {
        snprintf(lang, sizeof(lang), ",\"lang\":\"%s\"", cfg->lang);
    }
    // The receiver answers with HELLO listing what it understands: "rects":true = partial screen updates. The
    // message also carries this program's version, which the receiver page shows next to its own, and the language
    // this program is set to, which the page shows its texts in.
    char json[192];
    snprintf(json, sizeof(json),
             "{\"type\":\"CAPS\",\"rects\":1,\"ver\":\"" NATIVE_APP_VERSION "\",\"build\":\"" __DATE__ " " __TIME__
             "\"%s}",
             lang);
    return send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
}

// Opens the plain TCP connection for the pictures (see CastClient.direct_port).
static bool direct_open(CastClient *client, const CastConfig *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)client->direct_port);
    if (inet_pton(AF_INET, cfg->tv_ip, &addr.sin_addr) <= 0) {
        close(fd);
        return false;
    }
    int res = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        return false;
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    bool connected = false;
    for (int step = 0; step < 10 && !client_cancelled(client); step++) {
        int ready = poll(&pfd, 1, 200);
        if (ready > 0) {
            connected = true;
            break;
        }
        if (ready < 0 && errno != EINTR) {
            break;
        }
    }
    if (!connected) {
        close(fd);
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = SEND_BUFFER_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    client->direct_fd = fd;
    return true;
}

// Sends everything on the direct connection (the time counts as network and as "write", there is no TLS in it).
static bool direct_send_all(CastClient *client, const uint8_t *data, size_t len) {
    size_t sent = 0;
    int retries = 0;
    while (sent < len) {
        if (client_cancelled(client)) {
            return false;
        }
#ifdef __3DS__
        uint64_t start = svcGetSystemTick();
#endif
        ssize_t r = send(client->direct_fd, data + sent, len - sent, 0);
#ifdef __3DS__
        uint64_t spent = svcGetSystemTick() - start;
        cast_net_ticks += spent;
        cast_write_ticks += spent;
#endif
        if (r > 0) {
            sent += (size_t)r;
            retries = 0;
        } else if (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)) {
            if (++retries > 250) {
                return false;
            }
            struct pollfd pfd = {client->direct_fd, POLLOUT, 0};
            poll(&pfd, 1, 20);
        } else {
            return false;
        }
    }
    return true;
}

bool cast_client_send_rects(CastClient *client, const CastConfig *cfg, const CastRect *rects, unsigned count) {
    if (client->state != CAST_STATE_STREAMING || client->transport_id[0] == '\0' || !rects || !count) {
        return false;
    }
    size_t total = 0;
    for (unsigned k = 0; k < count; k++) {
        if (!rects[k].b64 || !rects[k].len) {
            return false;
        }
        total += (rects[k].len + CAST_PART_CHARS - 1) / CAST_PART_CHARS;
    }
    char cj[160];
    cfg_json(cj, sizeof(cj), cfg);
    // The message is built after 4 spare bytes: on the direct connection they hold its length.
    char *buffer = (char *)malloc(CAST_PART_CHARS + 516);
    if (!buffer) {
        return false;
    }
    char *json = buffer + 4;
    // Not reachable at all (a firewall, say): everything stays on the Cast connection. One that worked and then broke
    // (a Wi-Fi hiccup) is tried again after a few seconds, so a single hiccup does not end the direct pictures.
    if (client->direct_port && client->direct_fd < 0 &&
        (!client->direct_failed || (client->direct_was_ok && client->poll_ms >= client->direct_retry_at))) {
        if (direct_open(client, cfg)) {
            client->direct_failed = false;
        } else {
            client->direct_failed = true;
            client->direct_retry_at = client->poll_ms + DIRECT_RETRY_MS;
        }
    }
    bool ok = true;
    size_t message = 0;
    for (unsigned k = 0; k < count && ok; k++) {
        const CastRect *r = &rects[k];
        size_t parts = (r->len + CAST_PART_CHARS - 1) / CAST_PART_CHARS;
        for (size_t i = 0; i < parts && ok; i++) {
            size_t off = i * CAST_PART_CHARS, n = r->len - off;
            if (n > CAST_PART_CHARS) {
                n = CAST_PART_CHARS;
            }
            message++;
#ifdef __3DS__
            uint64_t build_start = svcGetSystemTick();
#endif
            int h = snprintf(
                json, 512,
                "{\"type\":\"RECT\",\"s\":\"%s\",\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"i\":%u,\"n\":%u%s%s%s,\"d\":\"",
                r->screen ? "bot" : "top", r->x, r->y, r->w, r->h, (unsigned)i, (unsigned)parts,
                message == 1 ? ",\"cfg\":" : "", message == 1 ? cj : "", message < total ? ",\"cont\":1" : "");
            if (h <= 0 || h >= 512) {
                ok = false;
                break;
            }
            memcpy(json + h, r->b64 + off, n);
            memcpy(json + h + n, "\"}", 3);
#ifdef __3DS__
            cast_build_ticks += svcGetSystemTick() - build_start;
#endif
            bool sent_direct = false;
            if (client->direct_fd >= 0) {
                uint32_t length = (uint32_t)(h + n + 2);
                buffer[0] = (char)(length >> 24);
                buffer[1] = (char)(length >> 16);
                buffer[2] = (char)(length >> 8);
                buffer[3] = (char)length;
                sent_direct = direct_send_all(client, (const uint8_t *)buffer, 4 + (size_t)length);
                if (!sent_direct) {
                    // The stream is cut in the middle of a message (the receiver drops the unfinished one): close the
                    // connection and send this message, and all after it, over the Cast one.
                    direct_close(client);
                    client->direct_failed = true;
                    client->direct_retry_at = client->poll_ms + DIRECT_RETRY_MS;
                } else {
                    client->direct_was_ok = true;
                }
            }
            ok = sent_direct || send_cast_packet(client, client->transport_id, CAST_NS_CUSTOM, json);
        }
    }
    free(buffer);
    return ok;
}
