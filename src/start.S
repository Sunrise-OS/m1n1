/* SPDX-License-Identifier: MIT */

#include "soc.h"

#define UTRSTAT 0x010
#define UTXH 0x020

.extern _start_c
.extern _stack_bot
.extern _v_sp0_sync
.extern _v_sp0_irq
.extern _v_sp0_fiq
.extern _v_sp0_serr
.extern _reset_stack
.extern _cpu_reset_c
.extern wdt_reboot

.section .init, "ax"

#ifdef PLATFORM_GS201
/*
 * ABL (Pixel 7a bootloader) requires a standard Linux ARM64 Image header
 * at offset 0 of the kernel payload; without the "ARM\x64" magic at offset
 * 0x38, ABL rejects the image and returns to fastboot mode.
 */
.globl _image_head
_image_head:
    b _start                    /* code0: branch to executable start */
    .long 0                     /* code1 */
    .quad 0                     /* text_offset: image load offset (LE) */
    .quad 0x200000              /* image_size: effective size */
    .quad 0xa                   /* flags: 2MB aligned, any page size */
    .quad 0                     /* res2 */
    .quad 0                     /* res3 */
    .quad 0                     /* res4 */
    .ascii "ARM\x64"            /* magic */
    .long 0                     /* res5 */

    /* Pad to 0x800 (2048 bytes) so _vectors_start retains its 2 KiB alignment */
    .balign 2048
#endif

.align 11
.globl _vectors_start
_vectors_start:

    mov x9, '0'
#ifdef PLATFORM_GS201
    b _start
#else
    b cpu_reset
#endif
    .align 7
    mov x9, '1'
    b exc_unk
    .align 7
    mov x9, '2'
    b exc_unk
    .align 7
    mov x9, '3'
    b exc_unk
    .align 7
    b _v_sp0_sync
    .align 7
    b _v_sp0_irq
    .align 7
    b _v_sp0_fiq
    .align 7
    b _v_sp0_serr
    .align 7
    b _v_sp0_sync
    .align 7
    b _v_sp0_irq
    .align 7
    b _v_sp0_fiq
    .align 7
    b _v_sp0_serr
    .align 7
    mov x9, 'p'
    b exc_unk
    .align 7
    mov x9, 'q'
    b exc_unk
    .align 7
    mov x9, 'r'
    b exc_unk
    .align 7
    mov x9, 's'
    b exc_unk
    .align 7

.globl _start
.type _start, @function
_start:
#ifdef PLATFORM_GS201
    /*
     * ABL enters us with the bootloader's FDT in x0 and both CPU-cluster
     * watchdogs armed.  Disarm them and start from a known translation/cache
     * state before touching DRAM: ABL may leave the MMU on with stale tables,
     * and m1n1 relocates itself at whatever physical address it was loaded at.
     */
    mrs x8, CurrentEL
    lsr x8, x8, #2

    /* Disarm WTCON on cluster 0 and 1 */
    movz x9, #0x0000
    movk x9, #0x1006, lsl #16
    str wzr, [x9]
    movz x9, #0x0000
    movk x9, #0x1007, lsl #16
    str wzr, [x9]

    /*
     * Disarm PMU watchdog counter enables (NONCPU_OUT) and unmask reset:
     * PMU base: 0x18060000
     * 0x1220: CLUSTER0_NONCPU_OUT (bit 8 = wdt cnt en)
     * 0x1420: CLUSTER1_NONCPU_OUT (bit 7 = wdt cnt en)
     * 0x1244: CLUSTER0_NONCPU_INT_EN
     * 0x1444: CLUSTER1_NONCPU_INT_EN
     */
    movz x9, #0x1806, lsl #16
    movk x9, #0x1220
    str wzr, [x9]
    movz x9, #0x1806, lsl #16
    movk x9, #0x1420
    str wzr, [x9]
    movz x9, #0x1806, lsl #16
    movk x9, #0x1244
    str wzr, [x9]
    movz x9, #0x1806, lsl #16
    movk x9, #0x1444
    str wzr, [x9]

    cmp x8, #3
    b.eq .Lgs201_el3
    cmp x8, #2
    b.eq .Lgs201_el2
    mrs x9, sctlr_el1
    b .Lgs201_clr
.Lgs201_el2:
    mrs x9, sctlr_el2
    b .Lgs201_clr
.Lgs201_el3:
    mrs x9, sctlr_el3
.Lgs201_clr:
    movz x10, #0x1005 /* M | C | I */
    bic x9, x9, x10
    cmp x8, #3
    b.eq .Lgs201_w3
    cmp x8, #2
    b.eq .Lgs201_w2
    msr sctlr_el1, x9
    b .Lgs201_done
.Lgs201_w2:
    msr sctlr_el2, x9
    b .Lgs201_done
.Lgs201_w3:
    msr sctlr_el3, x9
.Lgs201_done:
    isb
    ic iallu
    dsb sy
    isb
#endif
    mov x19, x0

    mov w0, 'm'
    bl debug_putc

    /*
     * On GS201, m1n1 is booted by ABL at an arbitrary load address in DRAM.
     * Before apply_rela runs, PC-relative addressing works (adrp/add),
     * but we must calculate SP relative to our current runtime base!
     */
    adrp x1, _stack_bot
    add x1, x1, :lo12:_stack_bot
    mov sp, x1

    mov w0, '1'
    bl debug_putc

    ldr x2, [sp, #-8]

    mov w0, 'n'
    bl debug_putc

    adrp x0, _base
    mov x20, x0
    adrp x1, _rela_start
    add x1, x1, :lo12:_rela_start
    adrp x2, _rela_end
    add x2, x2, :lo12:_rela_end
    bl apply_rela

    mov w0, '1'
    bl debug_putc
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    bl pan_fixup

    mrs x8, CurrentEL
    cmp x8, #0xc
    beq start_el3

    mov x0, x19
    mov x1, x20
#ifdef PLATFORM_GS201
    bl gs201_main
#else
    bl _start_c
#endif
    b .

start_el3:
    bl exception_initialize
    adrp x8, _stack_bot_el3
    mov sp, x8
    mov x0, x19
    mov x1, x20
    msr tpidr_el3, xzr
    adrp x6, _stack_bot
    adr x7, _start_c
    b el3_eret_to_el1

.globl exc_unk
.type exc_unk, @function
exc_unk:
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    mov w0, '!'
    bl debug_putc
    mov w0, 'E'
    bl debug_putc
    mov w0, 'x'
    bl debug_putc
    mov w0, 'C'
    bl debug_putc
    mov w0, ':'
    bl debug_putc
    mov w0, w9
    bl debug_putc
    mov w0, '!'
    bl debug_putc
    mov w0, 0xd /* '\r', clang compat */
    bl debug_putc
    mov w0, '\n'
    bl debug_putc
    b reboot

.globl cpu_reset
.type cpu_reset, @function
cpu_reset:
    mov w0, 'O'
    bl debug_putc

    adrp x1, _reset_stack
    add x1, x1, :lo12:_reset_stack
    ldr x1, [x1]
    mov sp, x1

    ldr x2, [sp, #-8]

    mov w0, 'K'
    bl debug_putc

    mov x0, sp

    mrs x8, CurrentEL
    cmp x8, #0xc
    beq cpu_reset_el3

    bl _cpu_reset_c
    b .

cpu_reset_el3:
    bl _cpu_reset_c
    adrp x6, _reset_stack_el1
    add x6, x6, :lo12:_reset_stack_el1
    ldr x6, [x6]
    mov x0, x6
    adr x7, _cpu_reset_c

el3_eret_to_el1:
    adr x8, _vectors_start
    msr sctlr_el1, xzr

    mrs x8, scr_el3
    orr x8, x8, #(1<<10) // EL1 execution state is AArch64
    orr x8, x8, #(1<<0)  // EL1 is non-secure
    msr scr_el3, x8
    msr elr_el3, x7 // Set EL1 entry point
    mov x8, #0x5 // EL1h
    msr spsr_el3, x8

    msr sp_el1, x6 // Set EL1 stack pointer

    eret

.globl debug_putc
.type debug_putc, @function
debug_putc:
#ifdef EARLY_UART_BASE
    ldr x1, =EARLY_UART_BASE

1:
    ldr w2, [x1, UTRSTAT]
    tst w2, #2
    beq 1b
    str w0, [x1, UTXH]
#endif
    ret

.globl reboot
.type reboot, @function
reboot:
    mrs x8, id_aa64pfr0_el1
    tst w8, 0xf00
    beq 1f
    mrs x0, CurrentEL
    cmp x0, #8
    beq 1f
    hvc #0
1:
    bl wdt_reboot
    b .

.pool
