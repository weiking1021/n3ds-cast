"""Exercise the real launcher, IPC replies, allocation and every rollback stage."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent
stub=Path('/tmp/native-launch-stubs');stub.mkdir(exist_ok=True)
header=r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uintptr_t u32;
typedef unsigned char u8;
typedef int s32;
typedef int64_t s64;
typedef int32_t Result;
typedef unsigned Handle;
typedef int ResourceLimitType;
enum {RESLIMIT_PRIORITY,RESLIMIT_COMMIT,RESLIMIT_THREAD,RESLIMIT_EVENT,RESLIMIT_MUTEX,
RESLIMIT_SEMAPHORE,RESLIMIT_TIMER,RESLIMIT_SHAREDMEMORY,RESLIMIT_ADDRESSARBITER,RESLIMIT_CPUTIME};
enum {MEMSTATE_FREE=0,MEMPERM_READWRITE=3,MEMOP_FREE=1,MEMOP_ALLOC=3,MEMOP_REGION_SYSTEM=0x200};
#define RS_NOTFOUND 4
#define R_SUMMARY(r) (((uint32_t)(r)>>21)&63)
#define IPC_MakeHeader(command,normal,translated) (((command)<<16)|((normal)<<6)|(translated))
typedef struct {u32 base_addr,size,perm,state;} MemInfo;
typedef struct {u32 flags;} PageInfo;
typedef struct {unsigned char name[8];uint16_t version,padding[3];u32 text_addr,text_size,ro_addr,ro_size,rw_addr,rw_size,text_size_total,ro_size_total,rw_size_total,padding2;uint64_t program_id;} CodeSetHeader;
typedef struct {int priority;u32 stack_size;int argc;void *argv,*envp;} StartupInfo;
u32 *getThreadCommandBuffer(void);
Result svcSendSyncRequest(Handle);
Result svcConnectToPort(Handle*,const char*);
Result svcCloseHandle(Handle);
Result svcGetSystemInfo(s64*,unsigned,unsigned);
Result svcQueryMemory(MemInfo*,PageInfo*,u32);
unsigned osGetKernelVersion(void);
Result svcCreateCodeSet(Handle*,const CodeSetHeader*,u32,u32,u32);
Result svcCreateProcess(Handle*,Handle,const u32*,unsigned);
Result svcCreateResourceLimit(Handle*);
Result svcGetProcessList(s32*,u32*,unsigned);
Result svcOpenProcess(Handle*,u32);
Result svcGetProcessInfo(s64*,Handle,unsigned);
Result svcGetResourceLimit(Handle*,Handle);
Result svcGetResourceLimitLimitValues(s64*,Handle,ResourceLimitType*,unsigned);
Result svcGetResourceLimitCurrentValues(s64*,Handle,ResourceLimitType*,unsigned);
Result svcSetResourceLimitValues(Handle,const ResourceLimitType*,const s64*,unsigned);
Result svcSetProcessResourceLimits(Handle,Handle);
Result svcSetProcessAffinityMask(Handle,const unsigned char*,unsigned);
Result svcSetProcessIdealProcessor(Handle,unsigned);
Result svcRun(Handle,const StartupInfo*);
Result svcWaitSynchronization(Handle,uint64_t);
void svcSleepThread(uint64_t);
Result svcTerminateProcess(Handle);
'''
(stub/'3ds.h').write_text(header)
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "client.h"
#include "resident_image.h"
static u32 command[128];
static unsigned stage,fail_at,allocated,freed,terminated,closed,waits;
static bool published,started,ready=true,foreign,quota;
static void *staging;
static Result step(void) {return ++stage==fail_at?-99:0;}
u32 *getThreadCommandBuffer(void) {return command;}
Result svcSendSyncRequest(Handle handle) {
    assert(handle==55);
    unsigned op=command[0]>>16;
    NativeStatus reply={.magic=foreign?0:NATIVE_MAGIC,.version=NATIVE_VERSION,.pid=100};
    strcpy(reply.message,"Ready");
    command[0]=IPC_MakeHeader(op,1+sizeof(reply)/4,0);command[1]=0;
    memcpy(command+2,&reply,sizeof(reply));return 0;
}
Result svcConnectToPort(Handle *out,const char *name) {
    assert(!strcmp(name,NATIVE_PORT));
    if(quota)return (Result)0xD8601837;
    if(!published || !ready)return (Result)0xD88007FA;
    *out=55;return 0;
}
Result svcCloseHandle(Handle handle) {assert(handle==55 || handle==90 || handle==100 || handle==110 || handle==23);closed++;return 0;}
Result svcGetSystemInfo(s64 *out,unsigned type,unsigned param) {assert(type==0x10000 && param==0);*out=1;return 0;}
Result svcQueryMemory(MemInfo *out,PageInfo *page,u32 address) {
    (void)page;assert(address==0x08000000 || address==0x09000000);
    *out=(MemInfo){.base_addr=address,.size=0x01000000,.state=address==0x08000000?5:0};return 0;
}
unsigned osGetKernelVersion(void) {return 0x023A0000;}
Result nativeSystemMemory(u32 *out,u32 address,u32 bytes,u32 op,u32 perm) {
    assert(bytes==NATIVE_IMAGE_SIZE);
    if((op&255)==MEMOP_FREE) {assert((void*)address==staging && perm==0);free(staging);staging=NULL;freed++;return 0;}
    assert(address==0x09000000 && op==(MEMOP_ALLOC|MEMOP_REGION_SYSTEM) && perm==3);
    Result rc=step();if(rc)return rc;
    staging=malloc(bytes);assert(staging);*out=(uintptr_t)staging;allocated++;return 0;
}
Result svcCreateCodeSet(Handle *out,const CodeSetHeader *h,u32 text,u32 ro,u32 rw) {
    assert(!memcmp(h->name,"n3d_cast",8) && !h->program_id);
    assert(text==(uintptr_t)staging && ro-text==NATIVE_RO_OFFSET && rw-text==NATIVE_RW_OFFSET);
    assert(h->text_addr==NATIVE_TEXT_ADDRESS && h->rw_size_total>=h->rw_size);
    assert(!memcmp(staging,native_image,NATIVE_IMAGE_SIZE));*out=90;return step();
}
Result svcCreateProcess(Handle *out,Handle cs,const u32 *caps,unsigned count) {
    assert(cs==90 && count==17 && caps[7]==0xFF002209);*out=100;return step();
}
Result svcCreateResourceLimit(Handle *out) {*out=110;return step();}
Result svcGetProcessList(s32 *count,u32 *pids,unsigned max) {assert(max==80);pids[0]=23;*count=1;return 0;}
Result svcOpenProcess(Handle *out,u32 pid) {assert(pid==23);*out=23;return 0;}
Result svcGetProcessInfo(s64 *out,Handle process,unsigned type) {assert(process==23 && type==0x10001);*out=0x0004013000002E02ULL;return 0;}
Result svcGetResourceLimit(Handle *out,Handle process) {assert(process==23);*out=110;return step();}
Result svcGetResourceLimitLimitValues(s64 *out,Handle handle,ResourceLimitType *names,unsigned count) {
    assert(handle==110 && count==6 && names[1]==RESLIMIT_COMMIT);
    for(unsigned i=0;i<6;i++)out[i]=100;
    out[0]=0x18;out[1]=32*1024*1024;return 0;
}
Result svcGetResourceLimitCurrentValues(s64 *out,Handle handle,ResourceLimitType *names,unsigned count) {
    assert(handle==110 && count==6 && names[1]==RESLIMIT_COMMIT);
    memset(out,0,sizeof(*out)*6);return step();
}
Result svcSetResourceLimitValues(Handle h,const ResourceLimitType *names,const s64 *values,unsigned count) {
    assert(h==110 && count==10 && names[1]==RESLIMIT_COMMIT && values[1]==NATIVE_MEMORY_LIMIT);return step();
}
Result svcSetProcessResourceLimits(Handle p,Handle r) {assert(p==100 && r==110);return step();}
Result svcSetProcessAffinityMask(Handle p,const unsigned char *mask,unsigned n) {assert(p==100 && *mask==2 && n==2);return step();}
Result svcSetProcessIdealProcessor(Handle p,unsigned core) {assert(p==100 && core==1);return step();}
Result svcRun(Handle p,const StartupInfo *info) {
    assert(p==100 && info->stack_size==65536);
    Result rc=step();if(!rc){published=true;started=true;}return rc;
}
Result svcWaitSynchronization(Handle p,uint64_t timeout) {assert(p==100 && timeout==0);waits++;return 0x09401BFE;}
void svcSleepThread(uint64_t timeout) {assert(timeout==10000000);}
Result svcTerminateProcess(Handle process) {assert(process==100 && started);terminated++;return 0;}
'''
tests=r'''
int main(void) {
    Handle client=0;NativeStatus status;char error[160];
    assert(native_launch(&client,&status,error,sizeof(error))==0 && client==55);
    assert(stage==9 && allocated==1 && freed==1 && !terminated && status.magic==NATIVE_MAGIC);
    stage=0;assert(native_launch(&client,&status,error,sizeof(error))==0 && !stage); // reuse own service
    published=true;foreign=true;client=0;stage=0;
    assert(native_launch(&client,&status,error,sizeof(error))!=0 && !stage && !client);
    foreign=false;quota=true;
    assert(native_launch(&client,&status,error,sizeof(error))==(Result)0xD8601837 && !stage);
    quota=false;
    for(unsigned failure=1;failure<=9;failure++) {
        stage=allocated=freed=terminated=0;published=started=false;fail_at=failure;client=0;
        assert(native_launch(&client,&status,error,sizeof(error))==-99);
        assert(allocated==freed && !staging && !terminated); // never terminate an unstarted/foreign process
        assert(strstr(error,"FFFFFF9D") || strstr(error,"FFFFFFFFFFFFFF9D"));
    }
    stage=allocated=freed=terminated=waits=0;published=started=false;fail_at=0;ready=false;
    assert(native_launch(&client,&status,error,sizeof(error))!=0);
    assert(allocated==freed && terminated==1 && waits==200);
    puts("PASS: actual launcher ready/reuse, foreign port and quota refusal, SYSTEM memory/persistent resource budget, all nine rollback stages and bounded startup timeout");
}
'''
Path('/tmp/native_launcher_test.c').write_text(code+'\n'+(root/'client.c').read_text()+'\n'+tests)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
    '-I'+str(stub),'-I'+str(root),'-I'+str(root/'build'),'-I'+str(root.parent/'shared'),
    '/tmp/native_launcher_test.c','-o','/tmp/native_launcher_test'],check=True)
subprocess.run(['/tmp/native_launcher_test'],check=True)
