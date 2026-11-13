#include "overlay.h"
#include <string.h>
#include "hw.h"
extern Result nativeProcessPhysical(Handle, unsigned, uint32_t *, uint32_t);
extern Result nativeMapPixels(Handle, u32, Handle, uint32_t, uint32_t, uint32_t);
extern Result nativeUnmapPixels(Handle, u32, uint32_t);
static uint32_t reg(unsigned offset) {
    return native_hw_read(0x1EF00500 + offset);
}
// No APT/NS IPC here. On hardware, APT:A GetAppletManInfo/GetAppletInfo from
// this unregistered background process made NS stop with svcBreak at stream
// start. Only the live game process (kernel query) gates drawing.
static bool foreground(NativeOverlay *o) {
    return native_capture_game(&o->capture);
}
static bool physical(Handle game, uint32_t va, uint32_t pa, size_t bytes) {
    uint32_t first = 0, last = 0;
    return nativeProcessPhysical(game, 4, &first, va) == 0 && first == pa &&
           nativeProcessPhysical(game, 4, &last, va + bytes - 1) == 0 && last == pa + bytes - 1;
}
static bool selected(const NativeFrame *f) {
    return (reg(0x78) & 1) == f->bank && reg(f->bank ? 0x6C : 0x68) == f->address && (reg(0x70) & 7) == f->format &&
           reg(0x90) == f->stride;
}
static bool rosalina_frame(const NativeFrame *f) {
    return f->address == 0x1848F000 && f->format == 2 && f->stride == 480 && reg(0x68) == reg(0x6C);
}
static bool still_owned(NativeOverlay *o, NativeOverlayBuffer *b) {
    if (!o->capture.game || svcWaitSynchronization(o->capture.game, 0) != (Result)0x09401BFE) {
        return false;
    }
    if (b->mapping) {
        return physical(o->capture.game, b->source, b->frame.address, b->size);
    }
    return reg(b->frame.bank ? 0x6C : 0x68) == b->frame.address && (reg(0x70) & 7) == b->frame.format &&
           reg(0x90) == b->frame.stride && !rosalina_frame(&b->frame);
}
static void release(NativeOverlay *o, NativeOverlayBuffer *b, bool restore) {
    if (b->pixels && b->saved.valid && restore && still_owned(o, b)) {
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)b->pixels, b->size);
        native_overlay_restore(&b->saved, &b->frame, b->pixels, b->size);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)b->pixels, b->size);
    }
    if (b->mapping) {
        Result rc = nativeUnmapPixels(CUR_PROCESS_HANDLE, (u32)b->mapping, b->mapped_size);
        if (rc != 0) {
            o->error = rc;
            svcExitProcess();
        } // Never free an address reservation that is still mapped.
        mappableFree(b->mapping);
    }
    memset(b, 0, sizeof(*b));
}
void native_overlay_close(NativeOverlay *o) {
    // release() restores only while still_owned() proves the buffer is unchanged.
    bool restore = o->capture.game && foreground(o);
    for (unsigned i = 0; i < 2; i++) {
        release(o, &o->buffers[i], restore);
    }
    native_capture_close(&o->capture);
    o->pid = 0;
}
unsigned native_overlay_update(NativeOverlay *o, const char *label) {
    if (!foreground(o)) {
        for (unsigned i = 0; i < 2; i++) {
            release(o, &o->buffers[i], false);
        }
        native_capture_close(&o->capture);
        return 1;
    }
    if (o->pid != o->capture.pid) {
        for (unsigned i = 0; i < 2; i++) {
            release(o, &o->buffers[i], false);
        }
        o->pid = o->capture.pid;
    }
    NativeFrame f = {0};
    f.bank = reg(0x78) & 1;
    f.address = reg(f.bank ? 0x6C : 0x68);
    f.width = (reg(0x5C) >> 16) & 0x3FF;
    f.format = reg(0x70) & 7;
    f.stride = reg(0x90);
    size_t bytes = native_frame_span(&f);
    if (f.width != 320 || !bytes || rosalina_frame(&f)) {
        return 1;
    }
    NativeOverlayBuffer *b = &o->buffers[f.bank];
    if (b->pixels && (b->frame.address != f.address || b->frame.format != f.format || b->frame.stride != f.stride)) {
        release(o, b, true);
    }
    if (!b->pixels) {
        b->source = 0;
        b->mapped_size = 0;
        b->frame = f;
        b->size = bytes;
        if (f.address >= 0x18000000 && (uint64_t)f.address + bytes <= 0x18600000) {
            b->pixels = (uint8_t *)(0x1F000000 + (f.address - 0x18000000));
        } else if (f.address >= 0x20000000 && (uint64_t)f.address + bytes <= 0x30000000) {
            uint32_t offset = f.address - 0x20000000, candidates[] = {0x30000000 + offset, 0x14000000 + offset};
            for (unsigned i = 0; i < 2; i++) {
                if (physical(o->capture.game, candidates[i], f.address, bytes)) {
                    b->source = candidates[i];
                    break;
                }
            }
            if (!b->source) {
                return 2;
            }
            uint32_t page = b->source & ~0xFFFu, skip = b->source - page;
            b->mapped_size = (bytes + skip + 0xFFF) & ~0xFFFu;
            b->mapping = mappableAlloc(b->mapped_size);
            if (!b->mapping) {
                o->error = (Result)-1;
                return 2;
            }
            Result rc = nativeMapPixels(CUR_PROCESS_HANDLE, (u32)b->mapping, o->capture.game, page, b->mapped_size, 0);
            if (rc != 0) {
                mappableFree(b->mapping);
                b->mapping = NULL;
                o->error = rc;
                return 2;
            }
            b->pixels = (uint8_t *)b->mapping + skip;
        } else {
            return 2;
        }
    }
    b->frame = f;
    if (!still_owned(o, b)) {
        release(o, b, false);
        return 1;
    }
    if (!selected(&f)) {
        return 1;
    }
    Result rc = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)b->pixels, bytes);
    if (rc != 0) {
        o->error = rc;
        return 2;
    }
    if (!selected(&f)) {
        return 1;
    }
    if (!native_overlay_paint(&b->saved, &f, b->pixels, bytes, label)) {
        return 2;
    }
    rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)b->pixels, bytes);
    if (rc != 0) {
        o->error = rc;
        return 2;
    }
    o->error = 0;
    return 0;
}
