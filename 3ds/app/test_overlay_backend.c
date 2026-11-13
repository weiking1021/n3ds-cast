// Tests the real overlay.c (the status text drawn into the game's framebuffer) against fake kernel services and
// fake display registers. test.sh compiles it with -D__3DS__ -Itest_stubs.
// Deliberately no srvGetServiceHandle/svcSendSyncRequest stubs: overlay.c must not use APT/NS IPC.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "overlay.h"

#define OVERLAY_REGISTER_BASE 0x1EF00500u
#define GAME_HANDLE 23

static uint32_t regs[64];
static unsigned maps, unmaps, flushes;
static bool game_alive = true;
static int map_error;
static bool flip;

unsigned native_hw_read(unsigned address) {
    assert(address >= OVERLAY_REGISTER_BASE && address < OVERLAY_REGISTER_BASE + sizeof(regs));
    return regs[(address - OVERLAY_REGISTER_BASE) / 4];
}
bool native_capture_game(NativeCapture *c) {
    if (!game_alive) {
        memset(c, 0, sizeof(*c));
        return false;
    }
    c->game = GAME_HANDLE;
    c->pid = 7;
    c->title_id = 0x0004000000123400ULL;
    return true;
}
void native_capture_close(NativeCapture *c) {
    memset(c, 0, sizeof(*c));
}
Result svcWaitSynchronization(Handle h, s64 timeout) {
    assert(h == GAME_HANDLE && !timeout);
    return 0x09401BFE;
}
Result nativeProcessPhysical(Handle h, unsigned op, uint32_t *out, uint32_t va) {
    assert(h == GAME_HANDLE && op == 4);
    *out = va - 0x10000000;
    return 0;
}
void *mappableAlloc(size_t n) {
    void *p = malloc(n);
    memset(p, 0xA7, n);
    return p;
}
void mappableFree(void *p) {
    free(p);
}
Result nativeMapPixels(Handle dst, u32 dest, Handle src, uint32_t va, uint32_t size, uint32_t flags) {
    assert(dst == CUR_PROCESS_HANDLE && dest && src == GAME_HANDLE && va == 0x30001000 && size >= 320 * 720 && !flags);
    maps++;
    return map_error;
}
Result nativeUnmapPixels(Handle h, u32 dest, uint32_t size) {
    assert(h == CUR_PROCESS_HANDLE && dest && size);
    unmaps++;
    return 0;
}
Result svcInvalidateProcessDataCache(Handle h, u32 address, u32 size) {
    assert(h == CUR_PROCESS_HANDLE && address && size);
    if (flip) {
        regs[0x78 / 4] ^= 1;
    }
    return 0;
}
Result svcFlushProcessDataCache(Handle h, u32 address, u32 size) {
    assert(h == CUR_PROCESS_HANDLE && address && size);
    flushes++;
    return 0;
}
__attribute__((noreturn)) void svcExitProcess(void) {
    abort();
}

int main(void) {
    NativeOverlay o = {0};
    regs[0x5C / 4] = 320u << 16 | 240;
    regs[0x68 / 4] = regs[0x6C / 4] = 0x20001000;
    regs[0x70 / 4] = 1;
    regs[0x90 / 4] = 720;
    assert(native_overlay_update(&o, "R  2 J 19 T 10 28FPS") == 0 && maps == 1 && flushes == 1);
    assert(o.buffers[0].saved.valid);
    uint8_t *p = o.buffers[0].pixels;
    native_overlay_close(&o);
    assert(unmaps == 1 && !o.buffers[0].pixels);
    game_alive = false;
    assert(native_overlay_update(&o, "R1") == 1 && maps == 1 && !o.buffers[0].pixels);
    game_alive = true;
    map_error = -5;
    assert(native_overlay_update(&o, "R1") == 2 && !o.buffers[0].mapping);
    map_error = 0;
    flip = true;
    assert(native_overlay_update(&o, "R1") == 1 && !o.buffers[0].saved.valid);
    flip = false;
    regs[0x78 / 4] = 0;
    assert(native_overlay_update(&o, "R1") == 0);
    p = o.buffers[0].pixels;
    native_overlay_restore(&o.buffers[0].saved, &o.buffers[0].frame, p, o.buffers[0].size);
    for (size_t i = 0; i < o.buffers[0].size; i++) {
        assert(p[i] == 0xA7);
    }
    native_overlay_close(&o);
    regs[0x68 / 4] = regs[0x6C / 4] = 0x1848F000;
    regs[0x70 / 4] = 2;
    regs[0x90 / 4] = 480;
    unsigned before = flushes;
    assert(native_overlay_update(&o, "R1") == 1 && flushes == before);
    regs[0x68 / 4] = 0x10000000;
    assert(native_overlay_update(&o, "R1") == 2 && flushes == before);
    native_overlay_close(&o);
    puts("PASS: actual overlay without APT/NS IPC, game-process guard, mapping failure, bank race, restoration, "
         "cleanup and Rosalina exclusion");
}
