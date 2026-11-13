#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#define NATIVE_TV_LIMIT 8
#define NATIVE_DISCOVERY_MAX_MS 6000
#define NATIVE_DISCOVERY_SETTLE_MS 1500
typedef struct {
    char name[64], ip[16];
} NativeTV;
typedef struct {
    char name[256], target[256], friendly[64];
    uint16_t port;
    int seen;
    uint8_t from[4];
    int has_from;
} NativeServiceRecord;
typedef struct {
    char name[256];
    uint8_t ip[4];
    int valid;
} NativeHostRecord;
typedef struct {
    NativeServiceRecord services[NATIVE_TV_LIMIT];
    NativeHostRecord hosts[16];
} NativeDNS;
int native_dns_packet(NativeDNS *cache, const uint8_t *packet, size_t bytes);
// Same, and remembers the sender address (4 bytes) as a fallback when no A record ever arrives.
int native_dns_packet_from(NativeDNS *cache, const uint8_t *packet, size_t bytes, const uint8_t *from);
unsigned native_dns_results(const NativeDNS *cache, NativeTV *out, unsigned capacity);
unsigned native_discover(NativeTV *out, unsigned capacity, const atomic_bool *cancelled);

typedef struct {
    unsigned packets, valid_packets, sockets;
    int error;
    bool multicast;
} NativeDiscoveryInfo;
unsigned native_discover_ex(NativeTV *out, unsigned capacity, const atomic_bool *cancelled, NativeDiscoveryInfo *info);
