#include <3ds.h>
#include <string.h>
#include "frame.h"
#include "hw.h"

// Luma3DS documented extended SVC ABI, implemented here without a framework.
extern Result nativeProcessPhysical(Handle process, unsigned operation, uint32_t *physical, uint32_t address);
static void va_cache_clear(void);
// Title id of the frontend when it is installed as a CIA (see cia/): never mistaken for the game.
#define NATIVE_CIA_TITLE_ID 0x000400000C3D5A00ULL
void native_capture_close(NativeCapture *c) {
    va_cache_clear();
    if (c->game) {
        svcCloseHandle(c->game);
    }
    c->game = 0;
    c->pid = 0;
    c->title_id = 0;
}
bool native_capture_game(NativeCapture *c) {
    if (c->game && svcWaitSynchronization(c->game, 0) == (Result)0x09401BFE) {
        return true;
    }
    native_capture_close(c);
    if (osGetTime() < c->next_search) {
        return false;
    }
    c->next_search = osGetTime() + 500;
    u32 pids[80];
    s32 count = 0;
    if (R_FAILED(svcGetProcessList(&count, pids, 80))) {
        return false;
    }
    if (count > 80) {
        count = 80;
    }
    for (int i = 0; i < count; i++) {
        Handle process = 0;
        s64 title = 0, name = 0;
        if (R_FAILED(svcOpenProcess(&process, pids[i]))) {
            continue;
        }
        Result rc = svcGetProcessInfo(&title, process, 0x10001);
        svcGetProcessInfo(&name, process, 0x10000);
        uint32_t kind = (uint64_t)title >> 32;
        if (R_SUCCEEDED(rc) && (kind == 0x00040000 || kind == 0x00040002) && memcmp(&name, "3dsx_app", 8) != 0 &&
            (uint64_t)title != NATIVE_CIA_TITLE_ID) {
            c->game = process;
            c->pid = pids[i];
            c->title_id = title;
            return true;
        }
        svcCloseHandle(process);
    }
    return false;
}
static bool physical_matches(Handle process, uint32_t address, uint32_t physical, size_t bytes) {
    // Padded, aligned outputs: never let a kernel-side write touch neighbouring stack slots.
    volatile uint32_t first[4] = {0}, last[4] = {0};
    if ((uint64_t)address + bytes > 0x100000000ULL || (uint64_t)physical + bytes > 0x30000000) {
        return false;
    }
    uint32_t a = 0, b = 0;
    if (R_FAILED(nativeProcessPhysical(process, 4, (uint32_t *)first, address))) {
        return false;
    }
    a = first[0];
    if (R_FAILED(nativeProcessPhysical(process, 4, (uint32_t *)last, address + bytes - 1))) {
        return false;
    }
    b = last[0];
    return a == physical && b == physical + bytes - 1;
}
static NativeFrame current_frame(bool top) {
    unsigned base = top ? 0x1EF00400 : 0x1EF00500;
    NativeFrame frame = {0};
    frame.width = (native_hw_read(base + 0x5C) >> 16) & 0x3FF;
    frame.bank = native_hw_read(base + 0x78) & 1;
    frame.address = native_hw_read(base + (frame.bank ? 0x6C : 0x68));
    frame.stride = native_hw_read(base + 0x90);
    frame.format = native_hw_read(base + 0x70) & 7;
    return frame;
}
// Verified source addresses. Checking a framebuffer costs two Luma ProcessPhysical calls; doing that for every
// frame (240 calls a second at 60 FPS, all aimed at a game process that may be starting or exiting) was
// the place a crash dump pointed at, and a framebuffer's mapping does not change while the game runs.
// So a verified (process, buffer) pair is reused and only re-checked every few seconds.
#define VA_CACHE_ENTRIES 8
#define VA_CACHE_MS 5000
static struct {
    Handle process;
    uint32_t address;
    size_t bytes;
    uint32_t va;
    uint64_t verified;
} va_cache[VA_CACHE_ENTRIES];
static unsigned va_cache_next;
static void va_cache_clear(void) {
    memset(va_cache, 0, sizeof(va_cache));
    va_cache_next = 0;
}
static bool linear_source_uncached(Handle process, const NativeFrame *frame, size_t bytes, uint32_t *address) {
    uint32_t offset = frame->address - 0x20000000;
    // Two public linear-mapping layouts. Never guess an unverified VA or
    // inspect/write a game's instructions, stacks, hooks or HID memory.
    uint32_t candidates[] = {0x30000000 + offset, 0x14000000 + offset};
    for (unsigned i = 0; i < 2; i++) {
        if (physical_matches(process, candidates[i], frame->address, bytes)) {
            *address = candidates[i];
            return true;
        }
    }
    return false;
}
static bool linear_source(Handle process, const NativeFrame *frame, size_t bytes, uint32_t *address) {
    if (!process || frame->address < 0x20000000 || (uint64_t)frame->address + bytes > 0x30000000) {
        return false;
    }
    uint64_t now = osGetTime();
    for (unsigned i = 0; i < VA_CACHE_ENTRIES; i++) {
        if (va_cache[i].process == process && va_cache[i].address == frame->address && va_cache[i].bytes == bytes) {
            if (now - va_cache[i].verified < VA_CACHE_MS) {
                *address = va_cache[i].va;
                return true;
            }
            va_cache[i].process = 0; // due for a fresh check
        }
    }
    if (!linear_source_uncached(process, frame, bytes, address)) {
        return false;
    }
    va_cache[va_cache_next].process = process;
    va_cache[va_cache_next].address = frame->address;
    va_cache[va_cache_next].bytes = bytes;
    va_cache[va_cache_next].va = *address;
    va_cache[va_cache_next].verified = now;
    va_cache_next = (va_cache_next + 1) % VA_CACHE_ENTRIES;
    return true;
}
// 1 = frame read, 0 = rejected, -1 = FCRAM framebuffer not mapped by `owner`,
// READ_TORN = the game flipped its framebuffer while it was being copied (internal: read_frame_retry maps it to 0).
#define READ_TORN 2
static int read_frame(Handle owner, bool top, NativeFrame *out, uint8_t *raw, size_t capacity) {
    if (!raw) {
        return 0;
    }
    unsigned base = top ? 0x1EF00400 : 0x1EF00500;
    NativeFrame frame = current_frame(top);
    size_t bytes = native_frame_span(&frame);
    if (!bytes || bytes > capacity) {
        return 0;
    }
    uint32_t address = 0;
    Handle source = 0;
    if (frame.address >= 0x18000000 && (uint64_t)frame.address + bytes <= 0x18600000) {
        source = CUR_PROCESS_HANDLE;
        address = 0x1F000000 + (frame.address - 0x18000000);
    } else if (frame.address >= 0x20000000 && (uint64_t)frame.address + bytes <= 0x30000000) {
        if (!linear_source(owner, &frame, bytes, &address)) {
            return -1;
        }
        source = owner;
    }
    if (!source) {
        return 0;
    }
    DmaConfig cfg = {0};
    cfg.channelId = -1;
    // No WAIT_AVAILABLE: skip if all channels are occupied, never stall a game.
    Handle dma = 0;
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)raw, bytes);
    if (R_FAILED(svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)raw, source, address, bytes, &cfg))) {
        return 0;
    }
    Result rc = svcWaitSynchronization(dma, 10000000); // bounded 10 ms
    if (rc != 0) {
        Result stopped = svcStopDma(dma);
        svcCloseHandle(dma);
        if (R_FAILED(stopped)) {
            svcExitProcess(); // do not reuse an active DMA destination
        }
        return 0;
    }
    svcCloseHandle(dma);
    if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)raw, bytes))) {
        return 0;
    }
    if ((native_hw_read(base + 0x78) & 1) != frame.bank) {
        return READ_TORN; // flipped during the copy
    }
    *out = frame;
    return 1;
}
// The most common rejection is a copy torn by the game flipping its framebuffer during the ~3 ms read (the
// bank check above). The buffer that was just flipped to is stable, so retrying at once nearly always works
// and saves the whole encode round that a rejected frame used to cost. Only that case is retried: a DMA timeout,
// busy channels or bad metadata are not (a retry would just burn up to 10 ms again).
static int read_frame_retry(Handle owner, bool top, NativeFrame *out, uint8_t *raw, size_t capacity) {
    int rc = read_frame(owner, top, out, raw, capacity);
    for (unsigned retry = 0; rc == READ_TORN && retry < 2; retry++) {
        rc = read_frame(owner, top, out, raw, capacity);
    }
    return rc == READ_TORN ? 0 : rc;
}
bool native_capture_frame(NativeCapture *c, bool top, NativeFrame *out, uint8_t *raw, size_t capacity) {
    return c->game && read_frame_retry(c->game, top, out, raw, capacity) == 1;
}
void native_capture_screen_close(NativeScreenCapture *s) {
    va_cache_clear();
    for (unsigned i = 0; i < 2; i++) {
        if (s->owner[i]) {
            svcCloseHandle(s->owner[i]);
        }
        s->owner[i] = 0;
        s->title_id[i] = 0;
    }
}
static bool find_owner(NativeScreenCapture *s, unsigned screen, bool top) {
    NativeFrame frame = current_frame(top);
    size_t bytes = native_frame_span(&frame);
    if (!bytes) {
        return false;
    }
    u32 pids[80];
    s32 count = 0;
    u32 self = 0;
    svcGetProcessId(&self, CUR_PROCESS_HANDLE);
    if (R_FAILED(svcGetProcessList(&count, pids, 80))) {
        return false;
    }
    if (count > 80) {
        count = 80;
    }
    for (int i = 0; i < count; i++) {
        if (pids[i] == self) {
            continue;
        }
        Handle process = 0;
        uint32_t address = 0;
        s64 title = 0;
        if (R_FAILED(svcOpenProcess(&process, pids[i]))) {
            continue;
        }
        // Only applications and applets (HOME, homebrew host, settings) can own a
        // framebuffer. Never probe kernel/system modules or a process that is
        // still starting or exiting: skip unless its title is readable and a UI kind.
        uint32_t kind = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&title, process, 0x10001))) {
            kind = (uint64_t)title >> 32;
        }
        bool ui =
            kind == 0x00040000 || kind == 0x00040002 || kind == 0x00040010 || kind == 0x00040030 || kind == 0x0004000E;
        if (ui && svcWaitSynchronization(process, 0) == (Result)0x09401BFE &&
            linear_source(process, &frame, bytes, &address)) {
            s->owner[screen] = process;
            s->title_id[screen] = (uint64_t)title;
            return true;
        }
        svcCloseHandle(process);
    }
    return false;
}
bool native_capture_screen(NativeScreenCapture *s, bool top, NativeFrame *out, uint8_t *raw, size_t capacity) {
    unsigned screen = top ? 0 : 1;
    if (s->owner[screen] && svcWaitSynchronization(s->owner[screen], 0) != (Result)0x09401BFE) {
        va_cache_clear();
        svcCloseHandle(s->owner[screen]);
        s->owner[screen] = 0;
        s->title_id[screen] = 0;
    }
    int rc = read_frame_retry(s->owner[screen], top, out, raw, capacity);
    if (rc >= 0) {
        return rc == 1;
    }
    // Framebuffer moved to another process (HOME, homebrew, applet): re-locate
    // its owner, rate-limited so a missing owner never spins the process list.
    if (osGetTime() < s->next_search[screen]) {
        return false;
    }
    s->next_search[screen] = osGetTime() + 500;
    if (s->owner[screen]) {
        svcCloseHandle(s->owner[screen]);
    }
    va_cache_clear();
    s->owner[screen] = 0;
    s->title_id[screen] = 0;
    return find_owner(s, screen, top) && read_frame(s->owner[screen], top, out, raw, capacity) == 1;
}
