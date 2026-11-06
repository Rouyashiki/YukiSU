/* Capture the bootstrap tail-call's external continuation before native retirement. */
.section .note.GNU-stack,"",%progbits
.text
.balign 4
.global zygisk_finalize_loader
.type zygisk_finalize_loader, %function
.hidden yz_finalize_native_loader
.hidden g_yz_ret_ctx
#if defined(__aarch64__)
zygisk_finalize_loader:
    hint #34
    adrp x16, g_yz_ret_ctx
    add x16, x16, :lo12:g_yz_ret_ctx
    stp x19, x20, [x16, #0]
    stp x21, x22, [x16, #16]
    stp x23, x24, [x16, #32]
    stp x25, x26, [x16, #48]
    stp x27, x28, [x16, #64]
    stp x29, x30, [x16, #80]
    mov x17, sp
    str x17, [x16, #96]
    stp d8, d9, [x16, #104]
    stp d10, d11, [x16, #120]
    stp d12, d13, [x16, #136]
    stp d14, d15, [x16, #152]
    b yz_finalize_native_loader
#elif defined(__arm__)
.syntax unified
.arm
zygisk_finalize_loader:
    ldr r12, 2f
1:  add r12, pc, r12
    stmia r12!, {r4-r11}
    str lr, [r12], #4
    str sp, [r12], #4
    vstmia r12, {d8-d15}
    b yz_finalize_native_loader
.balign 4
2:  .word g_yz_ret_ctx - (1b + 8)
#endif
.size zygisk_finalize_loader, .-zygisk_finalize_loader
