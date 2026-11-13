#include "discovery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t packet[2048];
static size_t used;
static void word(unsigned v) {
    packet[used++] = v >> 8;
    packet[used++] = v;
}
static void name(const char *s) {
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t length = dot ? (size_t)(dot - s) : strlen(s);
        packet[used++] = length;
        memcpy(packet + used, s, length);
        used += length;
        if (!dot) {
            break;
        }
        s = dot + 1;
    }
    packet[used++] = 0;
}
static size_t record(const char *owner, unsigned type) {
    name(owner);
    word(type);
    word(1);
    word(0);
    word(120);
    size_t length = used;
    word(0);
    return length;
}
static void finish(size_t length) {
    unsigned n = used - length - 2;
    packet[length] = n >> 8;
    packet[length + 1] = n;
}
static void begin(unsigned count) {
    memset(packet, 0, sizeof(packet));
    packet[2] = 0x84;
    packet[7] = count;
    used = 12;
}
int main(void) {
    NativeDNS cache = {0};
    NativeTV results[8];
    const char *instance = "My-TV._googlecast._tcp.local";
    begin(4);
    size_t r = record("_googlecast._tcp.local", 12);
    name(instance);
    finish(r);
    r = record(instance, 33);
    word(0);
    word(0);
    word(8009);
    name("tv-host.local");
    finish(r);
    r = record(instance, 16);
    packet[used++] = 14;
    memcpy(packet + used, "fn=Living Room", 14);
    used += 14;
    finish(r);
    r = record("tv-host.local", 1);
    packet[used++] = 192;
    packet[used++] = 168;
    packet[used++] = 1;
    packet[used++] = 42;
    finish(r);
    assert(native_dns_packet(&cache, packet, used));
    assert(native_dns_results(&cache, results, 8) == 1);
    assert(!strcmp(results[0].ip, "192.168.1.42") && !strcmp(results[0].name, "Living Room"));
    { // A long UTF-8 name ("A" and 22 three-byte characters = 67 bytes) is cut before an incomplete character, never inside one.
        NativeDNS longname = {0};
        char fn[3 + 67 + 1] = "fn=A";
        for (unsigned i = 0; i < 22; i++) {
            memcpy(fn + 4 + i * 3, "\xE9\x9B\xBB", 3);
        }
        begin(4);
        r = record("_googlecast._tcp.local", 12);
        name(instance);
        finish(r);
        r = record(instance, 33);
        word(0);
        word(0);
        word(8009);
        name("tv-host.local");
        finish(r);
        r = record(instance, 16);
        packet[used++] = 3 + 67;
        memcpy(packet + used, fn, 3 + 67);
        used += 3 + 67;
        finish(r);
        r = record("tv-host.local", 1);
        packet[used++] = 192;
        packet[used++] = 168;
        packet[used++] = 1;
        packet[used++] = 43;
        finish(r);
        assert(native_dns_packet(&longname, packet, used));
        assert(native_dns_results(&longname, results, 8) == 1);
        assert(strlen(results[0].name) == 61 && !memcmp(results[0].name + 58, "\xE9\x9B\xBB", 3));
    }
    // Every truncation is rejected without changing the cache.
    for (size_t bytes = 0; bytes < used; bytes++) {
        NativeDNS before = cache;
        assert(!native_dns_packet(&cache, packet, bytes));
        assert(!memcmp(&before, &cache, sizeof(cache)));
    }
    // Unrelated A responses cannot become TVs.
    NativeDNS unrelated = {0};
    begin(1);
    r = record("printer.local", 1);
    memset(packet + used, 1, 4);
    used += 4;
    finish(r);
    assert(native_dns_packet(&unrelated, packet, used));
    assert(native_dns_results(&unrelated, results, 8) == 0);
    // DNS compression cycle and bad RDATA lengths are bounded and rejected.
    begin(1);
    packet[used++] = 0xC0;
    packet[used++] = 12;
    assert(!native_dns_packet(&cache, packet, used));
    // Resolve across separate packets, including A arriving first.
    NativeDNS split = {0};
    begin(1);
    r = record("split.local", 1);
    packet[used++] = 10;
    packet[used++] = 0;
    packet[used++] = 0;
    packet[used++] = 2;
    finish(r);
    assert(native_dns_packet(&split, packet, used));
    begin(2);
    r = record("_googlecast._tcp.local", 12);
    name(instance);
    finish(r);
    r = record(instance, 33);
    word(0);
    word(0);
    word(8009);
    name("split.local");
    finish(r);
    assert(native_dns_packet(&split, packet, used));
    assert(native_dns_results(&split, results, 8) == 1);
    assert(!strcmp(results[0].ip, "10.0.0.2"));
    // No A record at all: the sender address of the packet carrying the SRV record is used (only via _from).
    NativeDNS noa = {0};
    const uint8_t sender[4] = {192, 168, 1, 77};
    begin(2);
    r = record("_googlecast._tcp.local", 12);
    name(instance);
    finish(r);
    r = record(instance, 33);
    word(0);
    word(0);
    word(8009);
    name("nohost.local");
    finish(r);
    assert(native_dns_packet(&noa, packet, used));
    assert(native_dns_results(&noa, results, 8) == 0);
    assert(native_dns_packet_from(&noa, packet, used, sender));
    assert(native_dns_results(&noa, results, 8) == 1);
    assert(!strcmp(results[0].ip, "192.168.1.77"));
    // A goodbye PTR removes the device.
    begin(1);
    r = record("_googlecast._tcp.local", 12);
    memset(packet + r - 4, 0, 4);
    name(instance);
    finish(r);
    assert(native_dns_packet(&split, packet, used));
    assert(native_dns_results(&split, results, 8) == 0);
    // A normal compressed response: PTR instance -> service suffix; SRV owner
    // -> PTR instance; A owner -> SRV hostname. Class cache-flush bits are legal.
    NativeDNS compressed = {0};
    begin(3);
    r = record("_googlecast._tcp.local", 12);
    unsigned instance_offset = used;
    packet[used++] = 4;
    memcpy(packet + used, "Room", 4);
    used += 4;
    word(0xC00C);
    finish(r);
    word(0xC000 | instance_offset);
    word(33);
    word(0x8001);
    word(0);
    word(120);
    r = used;
    word(0);
    word(0);
    word(0);
    word(8009);
    unsigned host_offset = used;
    name("compressed.local");
    finish(r);
    word(0xC000 | host_offset);
    word(1);
    word(0x8001);
    word(0);
    word(120);
    r = used;
    word(0);
    packet[used++] = 192;
    packet[used++] = 168;
    packet[used++] = 2;
    packet[used++] = 7;
    finish(r);
    assert(native_dns_packet(&compressed, packet, used));
    assert(native_dns_results(&compressed, results, 8) == 1);
    assert(!strcmp(results[0].ip, "192.168.2.7"));
    puts(
        "PASS: validated Cast PTR/SRV/TXT/A chain, split replies, friendly names, no printer false positives, TTL goodbye, compression cycle and truncation rollback");
}
