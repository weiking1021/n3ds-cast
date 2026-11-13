#pragma once
// The part of libctru that the host tests of 3ds/app need, declared only (the tests define what they call).
// Tests compile with -D__3DS__ -Itest_stubs so that <3ds.h> finds this file instead of the real one.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t u32; // wide enough to hold a host pointer, like the addresses the real code passes around
typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t s64;
typedef int32_t Result;
typedef unsigned Handle;
typedef void *Thread;
typedef int LightLock;

#define CUR_PROCESS_HANDLE 0xFFFF8001u
#define R_FAILED(result) ((result) < 0)
#define R_SUCCEEDED(result) ((result) >= 0)
#define IPC_MakeHeader(command, normal, translated) (((command) << 16) | ((normal) << 6) | (translated))

typedef struct {
    int channelId, endianSwapSize;
    unsigned flags;
} DmaConfig;

uint64_t osGetTime(void);
u32 *getThreadCommandBuffer(void);
void *mappableAlloc(size_t size);
void mappableFree(void *memory);

Result svcGetProcessId(u32 *out, Handle process);
Result svcWaitSynchronization(Handle handle, s64 nanoseconds);
Result svcCloseHandle(Handle handle);
Result svcGetProcessList(s32 *count, u32 *process_ids, s32 maximum);
Result svcOpenProcess(Handle *process, u32 process_id);
Result svcGetProcessInfo(s64 *out, Handle process, unsigned type);
Result svcInvalidateProcessDataCache(Handle process, u32 address, u32 size);
Result svcFlushProcessDataCache(Handle process, u32 address, u32 size);
Result svcStartInterProcessDma(Handle *dma, Handle destination_process, u32 destination, Handle source_process,
                               u32 source, u32 size, const DmaConfig *config);
Result svcStopDma(Handle dma);
Result svcAcceptSession(Handle *session, Handle port);
Result svcReplyAndReceive(s32 *index, const Handle *handles, s32 count, Handle reply_target);
Result svcSignalEvent(Handle event);
void svcSleepThread(s64 nanoseconds);
__attribute__((noreturn)) void svcExitProcess(void);

void LightLock_Init(LightLock *lock);
void LightLock_Lock(LightLock *lock);
int LightLock_TryLock(LightLock *lock);
void LightLock_Unlock(LightLock *lock);
