#include "../../../kernel/feature/yukizygisk/native_stub.h"

.section .text.start,"ax",%progbits
.global yz_native_start
.hidden yz_native_start
.type yz_native_start,%function
#if defined(__aarch64__)
yz_native_start:
  bti c
  sub sp, sp, #800
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  mrs x9, nzcv
  mrs x10, fpcr
  mrs x11, fpsr
  stp x9, x10, [sp, #248]
  str x11, [sp, #264]
  stp q0, q1, [sp, #288]
  stp q2, q3, [sp, #320]
  stp q4, q5, [sp, #352]
  stp q6, q7, [sp, #384]
  stp q8, q9, [sp, #416]
  stp q10, q11, [sp, #448]
  stp q12, q13, [sp, #480]
  stp q14, q15, [sp, #512]
  stp q16, q17, [sp, #544]
  stp q18, q19, [sp, #576]
  stp q20, q21, [sp, #608]
  stp q22, q23, [sp, #640]
  stp q24, q25, [sp, #672]
  stp q26, q27, [sp, #704]
  stp q28, q29, [sp, #736]
  stp q30, q31, [sp, #768]
  adr x19, yz_native_start
  add x19, x19, #YZ_NATIVE_STUB_DATA
  mov x0, x19
  ldr x16, [x19, #YZ_NS_RESOLVER]
  blr x16
  ldr x0, [x19, #YZ_NS_RESOLVER]
  ldr x1, [x19, #YZ_NS_LENGTH]
  mov x8, #215
  svc #0
  cbnz x0, 1f
  stp xzr, xzr, [x19, #YZ_NS_RESOLVER]
  b 2f
1:
  mov w0, #10
  str w0, [x19, #YZ_NS_STATUS]
  str xzr, [x19, #YZ_NS_BOOTSTRAP]
2:
  ldrsw x0, [x19, #YZ_NS_LOADER_FD]
  mov x8, #57
  svc #0
  ldr x16, [x19, #YZ_NS_BOOTSTRAP]
  cbz x16, 3f
  str xzr, [x19, #YZ_NS_BOOTSTRAP]
  ldr w0, [x19, #YZ_NS_CORE_FD]
  ldr w1, [x19, #YZ_NS_PACKET]
  blr x16
  ldr w9, [x19, #YZ_NS_ERROR]
  str w9, [x19, #YZ_NS_STATUS]
  mov w10, #200
  str w10, [x19, #YZ_NS_STATUS+4]
  ldr w9, [x19, #YZ_NS_OWNERSHIP]
  cmp w9, #4
  b.ne 6f
  ldr x0, [x19, #YZ_NS_LOADER]
  ldr x16, [x19, #YZ_NS_CLOSE]
  blr x16
  cbz w0, 7f
  mov w0, #11
  str w0, [x19, #YZ_NS_STATUS]
  b 5f
6:
  cmp w9, #2
  b.ne 5f
7:
  str xzr, [x19, #YZ_NS_LOADER]
  b 5f
3:
  ldr w20, [x19, #YZ_NS_COUNT]
  add x21, x19, #YZ_NS_FDS
4:
  cbz w20, 5f
  ldrsw x0, [x21], #4
  mov x8, #57
  svc #0
  sub w20, w20, #1
  b 4b
5:
  str xzr, [x19, #YZ_NS_CLOSE]
  ldrsw x0, [x19, #YZ_NS_REPORT_FD]
  add x1, x19, #YZ_NS_STATUS
  mov x2, #YZ_NS_REPORT_SIZE
  mov x8, #64
  svc #0
  ldrsw x0, [x19, #YZ_NS_REPORT_FD]
  mov x8, #57
  svc #0
  ldr x16, [x19, #YZ_NS_ENTRY]
  str x16, [sp, #128]
  ldp q0, q1, [sp, #288]
  ldp q2, q3, [sp, #320]
  ldp q4, q5, [sp, #352]
  ldp q6, q7, [sp, #384]
  ldp q8, q9, [sp, #416]
  ldp q10, q11, [sp, #448]
  ldp q12, q13, [sp, #480]
  ldp q14, q15, [sp, #512]
  ldp q16, q17, [sp, #544]
  ldp q18, q19, [sp, #576]
  ldp q20, q21, [sp, #608]
  ldp q22, q23, [sp, #640]
  ldp q24, q25, [sp, #672]
  ldp q26, q27, [sp, #704]
  ldp q28, q29, [sp, #736]
  ldp q30, q31, [sp, #768]
  ldp x9, x10, [sp, #248]
  ldr x11, [sp, #264]
  msr nzcv, x9
  msr fpcr, x10
  msr fpsr, x11
  ldp x0, x1, [sp, #0]
  ldp x2, x3, [sp, #16]
  ldp x4, x5, [sp, #32]
  ldp x6, x7, [sp, #48]
  ldp x8, x9, [sp, #64]
  ldp x10, x11, [sp, #80]
  ldp x12, x13, [sp, #96]
  ldp x14, x15, [sp, #112]
  ldp x16, x17, [sp, #128]
  ldp x18, x19, [sp, #144]
  ldp x20, x21, [sp, #160]
  ldp x22, x23, [sp, #176]
  ldp x24, x25, [sp, #192]
  ldp x26, x27, [sp, #208]
  ldp x28, x29, [sp, #224]
  ldr x30, [sp, #240]
  add sp, sp, #800
  br x16
#else
.syntax unified
.arm
.fpu neon
yz_native_start:
  push {r0-r12, lr}
  mrs r0, apsr
  vmrs r1, fpscr
  push {r0, r1}
  vpush {d0-d15}
  vpush {d16-d31}
  adr r4, yz_native_start
  add r4, r4, #YZ_NATIVE_STUB_DATA
  mov r0, r4
  ldr r12, [r4, #YZ_NS_RESOLVER]
  blx r12
  ldr r0, [r4, #YZ_NS_RESOLVER]
  ldr r1, [r4, #YZ_NS_LENGTH]
  mov r7, #91
  svc #0
  cmp r0, #0
  bne 1f
  mov r0, #0
  str r0, [r4, #YZ_NS_RESOLVER]
  str r0, [r4, #YZ_NS_LENGTH]
  b 2f
1:
  mov r0, #10
  str r0, [r4, #YZ_NS_STATUS]
  mov r0, #0
  str r0, [r4, #YZ_NS_BOOTSTRAP]
2:
  ldr r0, [r4, #YZ_NS_LOADER_FD]
  mov r7, #6
  svc #0
  ldr r12, [r4, #YZ_NS_BOOTSTRAP]
  cmp r12, #0
  beq 3f
  mov r0, #0
  str r0, [r4, #YZ_NS_BOOTSTRAP]
  ldr r0, [r4, #YZ_NS_CORE_FD]
  ldr r1, [r4, #YZ_NS_PACKET]
  blx r12
  ldr r0, [r4, #YZ_NS_ERROR]
  str r0, [r4, #YZ_NS_STATUS]
  mov r0, #200
  str r0, [r4, #YZ_NS_STATUS+4]
  ldr r0, [r4, #YZ_NS_OWNERSHIP]
  cmp r0, #4
  bne 6f
  ldr r0, [r4, #YZ_NS_LOADER]
  ldr r12, [r4, #YZ_NS_CLOSE]
  blx r12
  cmp r0, #0
  beq 7f
  mov r0, #11
  str r0, [r4, #YZ_NS_STATUS]
  b 5f
6:
  cmp r0, #2
  bne 5f
7:
  mov r0, #0
  str r0, [r4, #YZ_NS_LOADER]
  b 5f
3:
  ldr r5, [r4, #YZ_NS_COUNT]
  add r6, r4, #YZ_NS_FDS
4:
  cmp r5, #0
  beq 5f
  ldr r0, [r6], #4
  mov r7, #6
  svc #0
  sub r5, r5, #1
  b 4b
5:
  mov r0, #0
  str r0, [r4, #YZ_NS_CLOSE]
  ldr r0, [r4, #YZ_NS_REPORT_FD]
  add r1, r4, #YZ_NS_STATUS
  mov r2, #YZ_NS_REPORT_SIZE
  mov r7, #4
  svc #0
  ldr r0, [r4, #YZ_NS_REPORT_FD]
  mov r7, #6
  svc #0
  ldr r12, [r4, #YZ_NS_ENTRY]
  str r12, [sp, #312]
  vpop {d16-d31}
  vpop {d0-d15}
  pop {r0, r1}
  msr APSR_nzcvqg, r0
  vmsr fpscr, r1
  pop {r0-r12, lr}
  bx r12
#endif
.size yz_native_start, .-yz_native_start
.section .note.GNU-stack,"",%progbits
