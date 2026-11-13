#include "discovery.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

// Copies at most size-1 bytes of a UTF-8 string and never ends in the middle of a character (a name longer than the
// buffer is cut before its last, incomplete character).
static void copy_utf8(char *dst, size_t size, const char *src, size_t length) {
    size_t n = length < size - 1 ? length : size - 1;
    if (n < length) {
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static unsigned read16(const uint8_t *p) {
    return (unsigned)p[0] * 256 + p[1];
}
static int name_read(const uint8_t *p, size_t bytes, size_t *cursor, char out[256]) {
    size_t at = *cursor, used = 0, resume = 0;
    for (unsigned hops = 0; hops < 128; hops++) {
        if (at >= bytes) {
            return 0;
        }
        unsigned n = p[at++];
        if (!n) {
            out[used] = 0;
            *cursor = resume ? resume : at;
            return 1;
        }
        if ((n & 0xC0) == 0xC0) {
            if (at >= bytes) {
                return 0;
            }
            if (!resume) {
                resume = at + 1;
            }
            at = ((n & 63) << 8) | p[at];
            continue;
        }
        if ((n & 0xC0) || n > 63 || at + n > bytes || used + n + 1 >= 256) {
            return 0;
        }
        if (used) {
            out[used++] = '.';
        }
        for (unsigned i = 0; i < n; i++) {
            out[used++] = (char)tolower((unsigned char)p[at++]);
        }
    }
    return 0;
}
static int cast_name(const char *name) {
    const char suffix[] = "._googlecast._tcp.local";
    size_t n = strlen(name), s = sizeof(suffix) - 1;
    return n > s && !strcmp(name + n - s, suffix);
}
static NativeServiceRecord *service(NativeDNS *c, const char *name) {
    if (!cast_name(name)) {
        return NULL;
    }
    for (unsigned i = 0; i < NATIVE_TV_LIMIT; i++) {
        if (!strcmp(c->services[i].name, name)) {
            return &c->services[i];
        }
    }
    for (unsigned i = 0; i < NATIVE_TV_LIMIT; i++) {
        if (!c->services[i].name[0]) {
            snprintf(c->services[i].name, sizeof(c->services[i].name), "%.255s", name);
            return &c->services[i];
        }
    }
    return NULL;
}
static int parse_packet(NativeDNS *c, const uint8_t *p, size_t bytes, const uint8_t *from) {
    if (!c || !p || bytes < 12 || !(p[2] & 0x80) || (p[3] & 15)) {
        return 0;
    }
    size_t at = 12;
    char name[256], target[256];
    unsigned questions = read16(p + 4), records = read16(p + 6) + read16(p + 8) + read16(p + 10);
    if (questions > 64 || records > 256) {
        return 0;
    }
    for (unsigned i = 0; i < questions; i++) {
        if (!name_read(p, bytes, &at, name) || at + 4 > bytes) {
            return 0;
        }
        at += 4;
    }
    for (unsigned i = 0; i < records; i++) {
        if (!name_read(p, bytes, &at, name) || at + 10 > bytes) {
            return 0;
        }
        unsigned type = read16(p + at), klass = read16(p + at + 2) & 0x7FFF;
        unsigned length = read16(p + at + 8);
        bool alive = p[at + 4] || p[at + 5] || p[at + 6] || p[at + 7];
        at += 10;
        size_t end = at + length;
        if (end > bytes) {
            return 0;
        }
        if (klass != 1) {
            at = end;
            continue;
        }
        if (type == 12 && !strcmp(name, "_googlecast._tcp.local")) {
            size_t next = at;
            if (!name_read(p, bytes, &next, target) || next > end) {
                return 0;
            }
            NativeServiceRecord *s = service(c, target);
            if (s) {
                s->seen = alive;
            }
        } else if (type == 33 && length >= 7) {
            NativeServiceRecord *s = service(c, name);
            size_t next = at + 6;
            if (s && name_read(p, bytes, &next, target) && next <= end) {
                s->port = alive ? read16(p + at + 4) : 0;
                if (from && alive) {
                    memcpy(s->from, from, 4);
                    s->has_from = 1;
                }
                snprintf(s->target, sizeof(s->target), "%s", target);
            }
        } else if (type == 16) {
            NativeServiceRecord *s = service(c, name);
            size_t next = at;
            while (next < end) {
                unsigned n = p[next++];
                if (next + n > end) {
                    return 0;
                }
                if (s && alive && n >= 3 && !memcmp(p + next, "fn=", 3)) {
                    copy_utf8(s->friendly, sizeof(s->friendly), (const char *)p + next + 3, n - 3);
                }
                next += n;
            }
        } else if (type == 1 && length == 4) {
            NativeHostRecord *host = NULL;
            for (unsigned j = 0; j < 16; j++) {
                if (!strcmp(c->hosts[j].name, name)) {
                    host = &c->hosts[j];
                    break;
                }
            }
            if (!host) {
                for (unsigned j = 0; j < 16; j++) {
                    if (!c->hosts[j].name[0]) {
                        host = &c->hosts[j];
                        break;
                    }
                }
            }
            if (host) {
                snprintf(host->name, sizeof(host->name), "%s", name);
                memcpy(host->ip, p + at, 4);
                host->valid = alive;
            }
        }
        at = end;
    }
    return 1;
}
int native_dns_packet_from(NativeDNS *cache, const uint8_t *packet, size_t bytes, const uint8_t *from) {
    if (!cache) {
        return 0;
    }
    NativeDNS pending = *cache;
    if (!parse_packet(&pending, packet, bytes, from)) {
        return 0;
    }
    *cache = pending;
    return 1;
}
int native_dns_packet(NativeDNS *cache, const uint8_t *packet, size_t bytes) {
    return native_dns_packet_from(cache, packet, bytes, NULL);
}
unsigned native_dns_results(const NativeDNS *c, NativeTV *out, unsigned capacity) {
    unsigned count = 0;
    for (unsigned i = 0; i < NATIVE_TV_LIMIT && count < capacity; i++) {
        const NativeServiceRecord *s = &c->services[i];
        if (!s->seen || s->port != 8009 || !s->target[0]) {
            continue;
        }
        const uint8_t *ip = NULL;
        for (unsigned j = 0; j < 16; j++) {
            if (c->hosts[j].valid && !strcmp(s->target, c->hosts[j].name)) {
                ip = c->hosts[j].ip;
                break;
            }
        }
        // No A record yet (it often arrives in another packet, or never): the device that sent its own SRV
        // record is the device, so its source address will do.
        if (!ip && s->has_from) {
            ip = s->from;
        }
        if (!ip || !ip[0] || ip[0] >= 224 || ip[0] == 127) {
            continue;
        }
        snprintf(out[count].ip, sizeof(out[count].ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        int duplicate = 0;
        for (unsigned k = 0; k < count; k++) {
            if (!strcmp(out[k].ip, out[count].ip)) {
                duplicate = 1;
            }
        }
        if (duplicate) {
            continue;
        }
        const char *shown = s->friendly[0] ? s->friendly : s->name;
        copy_utf8(out[count].name, sizeof(out[count].name), shown, strlen(shown));
        count++;
    }
    return count;
}
static void query(int fd, const struct sockaddr_in *destination, const char *name, unsigned type, bool legacy) {
    uint8_t p[512] = {0};
    size_t at = 12;
    p[5] = 1;
    if (legacy) {
        p[0] = 0x4E;
        p[1] = 0x33;
    }
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (!n || n > 63 || at + n + 6 >= sizeof(p)) {
            return;
        }
        p[at++] = n;
        memcpy(p + at, name, n);
        at += n;
        if (!dot) {
            break;
        }
        name = dot + 1;
    }
    // Multicast-socket questions set the QU bit (unicast answer wanted): Wi-Fi loses multicast far more often.
    p[at++] = 0;
    p[at++] = type >> 8;
    p[at++] = type;
    p[at++] = legacy ? 0 : 0x80;
    p[at++] = 1;
    sendto(fd, p, at, 0, (const struct sockaddr *)destination, sizeof(*destination));
}

static int discovery_socket(bool multicast, NativeDiscoveryInfo *info) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        info->error = errno;
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = multicast ? htons(5353) : 0;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        info->error = errno;
        close(fd);
        return -1;
    }
    if (multicast) {
        struct ip_mreq membership = {0};
        inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
        membership.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
            info->error = errno;
            close(fd);
            return -1;
        }
        info->multicast = true;
    }
    int ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        info->error = errno;
        close(fd);
        return -1;
    }
    info->sockets++;
    return fd;
}
unsigned native_discover_ex(NativeTV *out, unsigned capacity, const atomic_bool *cancelled, NativeDiscoveryInfo *info) {
    NativeDiscoveryInfo unused = {0};
    if (!info) {
        info = &unused;
    }
    memset(info, 0, sizeof(*info));
    if (!out || !capacity || !cancelled) {
        return 0;
    }
    struct pollfd sockets[2] = {{.fd = discovery_socket(true, info), .events = POLLIN},
                                {.fd = discovery_socket(false, info), .events = POLLIN}};
    if (sockets[0].fd < 0 && sockets[1].fd < 0) {
        return 0;
    }
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &destination.sin_addr);
    NativeDNS cache = {0};
    struct timespec started, current;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        goto cleanup;
    }
    unsigned next_query = 0, known = 0;
    int64_t last_new = 0;
    for (unsigned step = 0; step < 2048 && !atomic_load(cancelled); step++) {
        if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
            break;
        }
        int64_t elapsed = (current.tv_sec - started.tv_sec) * 1000 + (current.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed < 0 || elapsed >= NATIVE_DISCOVERY_MAX_MS) {
            break;
        }
        // Stop once something was found and nothing new has appeared for a while; keep looking while empty.
        if (known && elapsed - last_new >= NATIVE_DISCOVERY_SETTLE_MS) {
            break;
        }
        if ((uint64_t)elapsed >= next_query) {
            // Lost packets are the usual failure: ask again quickly at first, then once a second.
            next_query = (unsigned)elapsed + (elapsed < 1500 ? 250 : 1000);
            for (unsigned sock = 0; sock < 2; sock++) {
                if (sockets[sock].fd >= 0) {
                    int fd = sockets[sock].fd;
                    bool legacy = sock == 1;
                    query(fd, &destination, "_googlecast._tcp.local", 12, legacy);
                    for (unsigned i = 0; i < NATIVE_TV_LIMIT; i++) {
                        if (cache.services[i].name[0]) {
                            NativeServiceRecord *s = &cache.services[i];
                            if (!s->target[0]) {
                                query(fd, &destination, s->name, 33, legacy);
                            }
                            if (!s->friendly[0]) {
                                query(fd, &destination, s->name, 16, legacy);
                            }
                            if (s->target[0]) {
                                query(fd, &destination, s->target, 1, legacy);
                            }
                        }
                    }
                }
            }
        }
        if (poll(sockets, 2, 50) > 0) {
            for (unsigned sock = 0; sock < 2; sock++) {
                if (!(sockets[sock].revents & POLLIN)) {
                    continue;
                }
                for (unsigned drain = 0; drain < 32 && !atomic_load(cancelled); drain++) {
                    uint8_t packet[9000];
                    struct sockaddr_in sender = {0};
                    socklen_t size = sizeof(sender);
                    ssize_t n =
                        recvfrom(sockets[sock].fd, packet, sizeof(packet), 0, (struct sockaddr *)&sender, &size);
                    if (n <= 0) {
                        break;
                    }
                    info->packets++;
                    if (sender.sin_port != htons(5353)) {
                        continue;
                    }
                    uint8_t from[4];
                    memcpy(from, &sender.sin_addr.s_addr, 4);
                    if (native_dns_packet_from(&cache, packet, (size_t)n, from)) {
                        info->valid_packets++;
                        // Follow up on a new service right away instead of waiting for the next round.
                        if (next_query > (unsigned)elapsed + 60) {
                            next_query = (unsigned)elapsed + 60;
                        }
                    }
                }
                NativeTV now[NATIVE_TV_LIMIT];
                unsigned total = native_dns_results(&cache, now, NATIVE_TV_LIMIT);
                if (total > known) {
                    known = total;
                    last_new = elapsed;
                }
            }
        }
    }
cleanup:
    for (unsigned sock = 0; sock < 2; sock++) {
        if (sockets[sock].fd >= 0) {
            close(sockets[sock].fd);
        }
    }
    return native_dns_results(&cache, out, capacity);
}
unsigned native_discover(NativeTV *out, unsigned capacity, const atomic_bool *cancelled) {
    return native_discover_ex(out, capacity, cancelled, NULL);
}
