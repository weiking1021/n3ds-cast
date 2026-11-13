.syntax unified
.arm
.text
.align 2
.global nativeProcessPhysical
.type nativeProcessPhysical,%function
nativeProcessPhysical:
    svc 0xB3
    bx lr

@ u32 *out, u32 address, u32 bytes, u32 operation, u32 permissions.
@ Luma extended ControlMemory ABI (r0=op,r1=VA,r2=alias,r3=size,
@ r4=permissions,r5=loader allocation). Only allocates our own memory.
.global nativeSystemMemory
.type nativeSystemMemory,%function
nativeSystemMemory:
    push {r4-r6,lr}
    mov r6,r0
    mov r0,r3
    mov r3,r2
    mov r2,#0
    ldr r4,[sp,#16]
    mov r5,#1
    svc 0xA2
    str r1,[r6]
    pop {r4-r6,pc}


@ Read/write alias of framebuffer pages in this process only. Luma A0 ABI:
@ r6=destination handle, r0=compatibility marker, r1/r2/r3/r4/r5=args.
.global nativeMapPixels
.type nativeMapPixels,%function
nativeMapPixels:
    push {r4-r6,lr}
    mov r6,r0
    mvn r0,#13
    ldr r4,[sp,#16]
    ldr r5,[sp,#20]
    svc 0xA0
    pop {r4-r6,pc}
.global nativeUnmapPixels
.type nativeUnmapPixels,%function
nativeUnmapPixels:
    svc 0xA1
    bx lr
