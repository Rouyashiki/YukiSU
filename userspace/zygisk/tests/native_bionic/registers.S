.text
.global native_register_probe
.hidden native_register_probe
.type native_register_probe,%function
#if defined(__aarch64__)
native_register_probe:
  stp x19, x20, [sp, #-32]!
  str x30, [sp, #16]
  mov x19, x0
  mov x20, sp
  movi v0.16b, #0x3d
  movi v31.16b, #0x5a
  mov x16, #0x60000000
  msr nzcv, x16
  mov x0, #0x10
  mov x1, #0x11
  mov x2, #0x12
  mov x3, #0x13
  mov x4, #0x14
  mov x5, #0x15
  mov x6, #0x16
  mov x7, #0x17
  mov x8, #0x18
  mov x9, #0x19
  mov x10, #0x1a
  mov x11, #0x1b
  mov x12, #0x1c
  mov x13, #0x1d
  mov x14, #0x1e
  mov x15, #0x1f
  mov x17, #0x21
  blr x19
  mrs x16, nzcv
  eor x16, x16, #0x60000000
  cbnz x16, 1f
  cmp sp, x20
  b.ne 1f
.macro check_register reg, value
  cmp \reg, #\value
  b.ne 1f
.endm
  check_register x0, 0x10
  check_register x1, 0x11
  check_register x2, 0x12
  check_register x3, 0x13
  check_register x4, 0x14
  check_register x5, 0x15
  check_register x6, 0x16
  check_register x7, 0x17
  check_register x8, 0x18
  check_register x9, 0x19
  check_register x10, 0x1a
  check_register x11, 0x1b
  check_register x12, 0x1c
  check_register x13, 0x1d
  check_register x14, 0x1e
  check_register x15, 0x1f
  check_register x17, 0x21
  movi v1.16b, #0x3d
  cmeq v0.16b, v0.16b, v1.16b
  uminv b0, v0.16b
  umov w16, v0.b[0]
  cmp w16, #255
  b.ne 1f
  movi v1.16b, #0x5a
  cmeq v31.16b, v31.16b, v1.16b
  uminv b31, v31.16b
  umov w16, v31.b[0]
  cmp w16, #255
  b.ne 1f
  mov w0, #1
  b 2f
1:
  mov w0, #0
2:
  ldr x30, [sp, #16]
  ldp x19, x20, [sp], #32
  ret
#else
.syntax unified
.arm
.fpu neon
native_register_probe:
  push {r4-r11, lr}
  sub sp, sp, #4
  mov r4, r0
  mov r5, sp
  vmov.i8 d0, #0x3d
  vmov.i8 d31, #0x5a
  mov r12, #0x60000000
  orr r12, r12, #0xa0000
  msr APSR_nzcvqg, r12
  mov r0, #0x10
  mov r1, #0x11
  mov r2, #0x12
  mov r3, #0x13
  blx r4
  mrs r12, apsr
  and r12, r12, #0xf0000000
  cmp r12, #0x60000000
  bne 1f
  mrs r12, apsr
  and r12, r12, #0xf0000
  cmp r12, #0xa0000
  bne 1f
  cmp sp, r5
  bne 1f
  cmp r0, #0x10
  bne 1f
  cmp r1, #0x11
  bne 1f
  cmp r2, #0x12
  bne 1f
  cmp r3, #0x13
  bne 1f
  vmov.i8 d1, #0x3d
  vceq.i8 d0, d0, d1
  vmov r0, r1, d0
  and r0, r0, r1
  cmn r0, #1
  bne 1f
  vmov.i8 d1, #0x5a
  vceq.i8 d31, d31, d1
  vmov r0, r1, d31
  and r0, r0, r1
  cmn r0, #1
  bne 1f
  mov r0, #1
  b 2f
1:
  mov r0, #0
2:
  add sp, sp, #4
  pop {r4-r11, pc}
#endif
.size native_register_probe, .-native_register_probe

.global native_test_resume
.hidden native_test_resume
.type native_test_resume,%function
#if defined(__arm__)
.thumb
.thumb_func
#endif
native_test_resume:
#if defined(__aarch64__)
  ret
#else
  bx lr
#endif
.size native_test_resume, .-native_test_resume
.section .note.GNU-stack,"",%progbits
