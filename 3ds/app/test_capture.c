// Tests the real capture.c against fake kernel services and fake display registers.
// test.sh compiles it with -D__3DS__ -Itest_stubs, so <3ds.h> is the declaration-only stub in test_stubs/.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "frame.h"
#include "hw.h"

#define REGISTER_BASE 0x1EF00000u
#define GAME_PROCESS 23
#define OTHER_PROCESS 22
#define DMA_HANDLE 99
#define TIMEOUT 0x09401BFE // svcWaitSynchronization: the process has not exited yet

static uint64_t time_now = 1000;
static bool cia_mode = false; // the frontend runs as an installed title (CIA) instead of the 3dsx host
static unsigned dma_calls, stop_calls, close_dma, close_game, physical_calls;
static int dma_wait = 0, game_wait = TIMEOUT;
static bool old_mapping = false, allow_game = true;
static int change_bank = 0; // number of upcoming DMA copies during which the game flips its framebuffer bank
static uint32_t regs[1024];

unsigned native_hw_read(unsigned address) {
    assert(address >= REGISTER_BASE && address < REGISTER_BASE + 0x1000);
    return regs[(address - REGISTER_BASE) / 4];
}
uint64_t osGetTime(void) {
    return time_now;
}
Result svcGetProcessId(u32 *out, Handle handle) {
    (void)handle;
    *out = 999;
    return 0;
}
Result svcWaitSynchronization(Handle handle, s64 timeout) {
    if (handle == DMA_HANDLE) {
        assert(timeout == 10000000);
        return dma_wait;
    }
    assert(handle == GAME_PROCESS && timeout == 0);
    return game_wait;
}
Result svcCloseHandle(Handle handle) {
    if (handle == DMA_HANDLE) {
        close_dma++;
    }
    if (handle == GAME_PROCESS) {
        close_game++;
    }
    return 0;
}
Result svcGetProcessList(s32 *count, u32 *pids, s32 maximum) {
    assert(maximum == 80);
    pids[0] = OTHER_PROCESS;
    pids[1] = GAME_PROCESS;
    *count = 2;
    return 0;
}
Result svcOpenProcess(Handle *process, u32 pid) {
    *process = (Handle)pid;
    return 0;
}
Result svcGetProcessInfo(s64 *out, Handle handle, unsigned type) {
    if (type == 0x10001) { // title id
        *out = (handle == OTHER_PROCESS && cia_mode) ? 0x000400000C3D5A00LL : allow_game ? 0x0004000000123400LL : 0;
    } else if (type == 0x10000) { // process name
        memcpy(out, handle == OTHER_PROCESS ? (cia_mode ? "n3ds-cas" : "3dsx_app") : "MS3DS\0\0\0", 8);
    } else {
        assert(0);
    }
    return 0;
}
Result nativeProcessPhysical(Handle process, unsigned operation, uint32_t *out, uint32_t address) {
    assert(process == GAME_PROCESS && operation == 4);
    physical_calls++;
    if (old_mapping && address >= 0x30000000) {
        *out = 0;
        return -1;
    }
    *out = 0x20000000 + (address - (old_mapping ? 0x14000000 : 0x30000000));
    return 0;
}
Result svcInvalidateProcessDataCache(Handle process, u32 address, u32 size) {
    assert(process == CUR_PROCESS_HANDLE && address && size);
    return 0;
}
Result svcStartInterProcessDma(Handle *out, Handle dst, u32 address, Handle src, u32 source, u32 size,
                               const DmaConfig *config) {
    assert(dst == CUR_PROCESS_HANDLE && (src == GAME_PROCESS || src == CUR_PROCESS_HANDLE));
    assert(config->channelId == -1 && config->flags == 0 && size <= 400 * 4096);
    assert(source == (src == GAME_PROCESS ? (old_mapping ? 0x14001000 : 0x30001000) : 0x1F001000));
    memset((void *)address, 0xA7, size);
    *out = DMA_HANDLE;
    dma_calls++;
    if (change_bank > 0) {
        regs[0x478 / 4] ^= 1;
        change_bank--;
    }
    return 0;
}
Result svcStopDma(Handle handle) {
    assert(handle == DMA_HANDLE);
    stop_calls++;
    return 0;
}
__attribute__((noreturn)) void svcExitProcess(void) {
    assert(!"unexpected forced exit");
    __builtin_trap();
}

int main(void) {
    { // an installed frontend (its own title id) is never taken for the game, whatever its process name
        NativeCapture x = {0};
        cia_mode = true;
        assert(native_capture_game(&x) && x.pid == GAME_PROCESS && x.title_id == 0x0004000000123400ULL);
        native_capture_close(&x);
        cia_mode = false;
        time_now += 1000;
        close_game = 0;
    }
    NativeCapture c = {0};
    assert(native_capture_game(&c) && c.game == GAME_PROCESS && c.pid == GAME_PROCESS);
    assert(native_capture_game(&c) && close_game == 0); // positive TIMEOUT means alive
    regs[0x45C / 4] = 400u << 16 | 240;
    regs[0x468 / 4] = 0x20001000;
    regs[0x470 / 4] = 1;
    regs[0x490 / 4] = 720;
    uint8_t raw[400 * 720 + 2];
    memset(raw, 0xD3, sizeof(raw));
    NativeFrame frame = {0};
    assert(native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2));
    assert(frame.width == 400 && frame.format == 1 && physical_calls == 2);
    assert(raw[0] == 0xD3 && raw[sizeof(raw) - 1] == 0xD3);
    assert(dma_calls == 1 && close_dma == 1 && !stop_calls);
    // A verified buffer is not re-verified for every frame (the two ProcessPhysical calls are cached for a few seconds).
    assert(native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2));
    assert(physical_calls == 2);
    old_mapping = true;
    time_now += 6000; // cache expired: the layout is checked again
    assert(native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2));
    assert(physical_calls == 5);
    regs[0x468 / 4] = 0x18001000;
    assert(native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2));
    assert(physical_calls == 5); // direct, read-only VRAM mapping
    dma_wait = TIMEOUT;
    assert(!native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2));
    assert(stop_calls == 1 && close_dma == 5); // positive timeout never treated as success
    dma_wait = 0;
    // The game flips its bank once during the copy: the torn read is rejected and the immediate retry succeeds.
    regs[0x46C / 4] = regs[0x468 / 4]; // the other bank's address register (real hardware always has both)
    unsigned calls0 = dma_calls;
    change_bank = 1;
    assert(native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2) && dma_calls == calls0 + 2);
    regs[0x478 / 4] = 0;
    regs[0x470 / 4] = 1;
    // It flips during every copy: bounded to one try plus two retries, then the frame is rejected.
    calls0 = dma_calls;
    change_bank = 1000;
    assert(!native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2) && dma_calls == calls0 + 3);
    change_bank = 0;
    regs[0x478 / 4] = 0;
    unsigned before = dma_calls;
    regs[0x470 / 4] = 7;
    assert(!native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2) && dma_calls == before);
    regs[0x470 / 4] = 1;
    regs[0x468 / 4] = 0x18600000;
    assert(!native_capture_frame(&c, true, &frame, raw + 1, sizeof(raw) - 2) && dma_calls == before);
    game_wait = 0;
    allow_game = false;
    time_now = 2000;
    assert(!native_capture_game(&c) && !c.game && close_game >= 1);
    puts("PASS: real backend game selection, frontend exclusion, positive TIMEOUT semantics, new/old linear mappings, "
         "VRAM, bounded DMA, channel contention policy, bank changes and invalid metadata");
}
