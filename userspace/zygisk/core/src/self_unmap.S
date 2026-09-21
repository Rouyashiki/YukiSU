.section .bss
#if defined(__aarch64__)
.align 3
.global g_yz_ret_ctx
.hidden g_yz_ret_ctx
g_yz_ret_ctx:
    .skip 21 * 8
#else
.align 3
.global g_yz_ret_ctx
.hidden g_yz_ret_ctx
g_yz_ret_ctx:
    .skip 10 * 4 + 8 * 8
#endif

.section .text
.align 2

#if defined(__aarch64__)
.global yz_cap_tmpl
.global yz_cap_tmpl_ctx
.global yz_cap_tmpl_wrap
.global yz_cap_tmpl_end
.hidden yz_cap_tmpl
.hidden yz_cap_tmpl_ctx
.hidden yz_cap_tmpl_wrap
.hidden yz_cap_tmpl_end
yz_cap_tmpl:
    adr  x16, yz_cap_tmpl_ctx
    ldr  x16, [x16]
    cbz  x16, 1f
    stp  x19, x20, [x16, #0]
    stp  x21, x22, [x16, #16]
    stp  x23, x24, [x16, #32]
    stp  x25, x26, [x16, #48]
    stp  x27, x28, [x16, #64]
    stp  x29, x30, [x16, #80]
    mov  x17, sp
    str  x17, [x16, #96]
    stp  d8, d9, [x16, #104]
    stp  d10, d11, [x16, #120]
    stp  d12, d13, [x16, #136]
    stp  d14, d15, [x16, #152]
1:
    adr  x16, yz_cap_tmpl_wrap
    ldr  x16, [x16]
    br   x16
.align 3
yz_cap_tmpl_ctx:
    .quad 0
yz_cap_tmpl_wrap:
    .quad 0
yz_cap_tmpl_end:

.global yz_self_unmap_tail
.hidden yz_self_unmap_tail
.type yz_self_unmap_tail, %function
yz_self_unmap_tail:
    adrp x16, g_yz_ret_ctx
    add  x16, x16, :lo12:g_yz_ret_ctx
    ldp  x19, x20, [x16, #0]
    ldp  x21, x22, [x16, #16]
    ldp  x23, x24, [x16, #32]
    ldp  x25, x26, [x16, #48]
    ldp  x27, x28, [x16, #64]
    ldp  x29, x30, [x16, #80]
    ldr  x17, [x16, #96]
    ldp  d8, d9, [x16, #104]
    ldp  d10, d11, [x16, #120]
    ldp  d12, d13, [x16, #136]
    ldp  d14, d15, [x16, #152]
    mov  sp, x17
    b    munmap
.size yz_self_unmap_tail, .-yz_self_unmap_tail
#else
.syntax unified

.arm
.global yz_cap_arm_tmpl
.global yz_cap_arm_tmpl_ctx
.global yz_cap_arm_tmpl_wrap
.global yz_cap_arm_tmpl_end
.hidden yz_cap_arm_tmpl
.hidden yz_cap_arm_tmpl_ctx
.hidden yz_cap_arm_tmpl_wrap
.hidden yz_cap_arm_tmpl_end
yz_cap_arm_tmpl:
    ldr  r12, yz_cap_arm_tmpl_ctx
    cmp  r12, #0
    beq  1f
    stmia r12!, {r4-r11}
    str  lr, [r12], #4
    str  sp, [r12], #4
    vstmia r12, {d8-d15}
1:
    ldr  r12, yz_cap_arm_tmpl_wrap
    bx   r12
.align 2
yz_cap_arm_tmpl_ctx:
    .word 0
yz_cap_arm_tmpl_wrap:
    .word 0
yz_cap_arm_tmpl_end:

.thumb
.thumb_func
.global yz_cap_thumb_tmpl
.global yz_cap_thumb_tmpl_ctx
.global yz_cap_thumb_tmpl_wrap
.global yz_cap_thumb_tmpl_end
.hidden yz_cap_thumb_tmpl
.hidden yz_cap_thumb_tmpl_ctx
.hidden yz_cap_thumb_tmpl_wrap
.hidden yz_cap_thumb_tmpl_end
yz_cap_thumb_tmpl:
    ldr.w r12, yz_cap_thumb_tmpl_ctx
    cmp.w r12, #0
    beq  1f
    stmia.w r12!, {r4-r11}
    str  lr, [r12], #4
    str  sp, [r12], #4
    vstmia r12, {d8-d15}
1:
    ldr.w r12, yz_cap_thumb_tmpl_wrap
    bx   r12
.align 2
yz_cap_thumb_tmpl_ctx:
    .word 0
yz_cap_thumb_tmpl_wrap:
    .word 0
yz_cap_thumb_tmpl_end:

.thumb
.thumb_func
.global yz_self_unmap_tail
.hidden yz_self_unmap_tail
.type yz_self_unmap_tail, %function
yz_self_unmap_tail:
    adr.w r12, 1f
    ldr  r2, [r12]
    add  r12, r2
    ldmia.w r12!, {r4-r11}
    ldr  lr, [r12], #4
    ldr  sp, [r12], #4
    vldmia r12, {d8-d15}
    b    munmap
.align 2
1:
    .word g_yz_ret_ctx - 1b
.size yz_self_unmap_tail, .-yz_self_unmap_tail
#endif
