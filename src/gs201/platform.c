/* SPDX-License-Identifier: MIT */

#include "gs201.h"

#include "exception.h"
#include "fb.h"
#include "heapblock.h"
#include "iodev.h"
#include "string.h"
#include "types.h"
#include "uart.h"
#include "uartproxy.h"
#include "utils.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

struct gs201_platform gs201;

extern char _bss_start[];
extern char _bss_end[];
extern char _payload_start[];
extern const char *const m1n1_version;

#define GS201_XNU_MEM_SIZE 0x40000000ULL /* 1 GiB, matches the U-Boot handoff */

#define MACH_O_MAGIC 0xfeedfacf

/*
 * Walk the FDT looking for the first node whose "compatible" contains want.
 * Returns 0 and fills reg_base/reg_size on success.
 */
static int fdt_find_compatible(const void *fdt, const char *want, u64 *reg_base, u64 *reg_size)
{
    int node, len;
    const char *compat;

    for (node = fdt_next_node(fdt, -1, NULL); node >= 0; node = fdt_next_node(fdt, node, NULL)) {
        compat = fdt_getprop(fdt, node, "compatible", &len);
        if (!compat)
            continue;

        for (const char *c = compat; c < compat + len; c += strlen(c) + 1) {
            const u32 *reg;
            int rlen, na, ns;

            if (strcmp(c, want))
                continue;

            reg = fdt_getprop(fdt, node, "reg", &rlen);
            if (!reg || rlen < 8)
                return -1;

            na = fdt_address_cells(fdt, fdt_parent_offset(fdt, node));
            ns = fdt_size_cells(fdt, fdt_parent_offset(fdt, node));
            if (na < 1 || na > 2 || ns < 1 || ns > 2 || rlen < (na + ns) * 4)
                return -1;

            u64 base = 0, size = 0;
            for (int i = 0; i < na; i++)
                base = (base << 32) | fdt32_to_cpu(reg[i]);
            for (int i = 0; i < ns; i++)
                size = (size << 32) | fdt32_to_cpu(reg[na + i]);

            *reg_base = base;
            *reg_size = size;
            return 0;
        }
    }

    return -1;
}

bool gs201_fdt_compatible(const char *compatible)
{
    const void *fdt = gs201.fdt;
    int node, len;
    const char *compat;

    if (!fdt || fdt_check_header(fdt))
        return false;

    for (node = fdt_next_node(fdt, -1, NULL); node >= 0; node = fdt_next_node(fdt, node, NULL)) {
        compat = fdt_getprop(fdt, node, "compatible", &len);
        if (!compat)
            continue;

        for (const char *c = compat; c < compat + len; c += strlen(c) + 1)
            if (!strcmp(c, compatible))
                return true;
    }

    return false;
}

static void fdt_parse_memory(const void *fdt)
{
    int node;

    for (node = fdt_first_subnode(fdt, 0); node >= 0; node = fdt_next_subnode(fdt, node)) {
        const char *name = fdt_get_name(fdt, node, NULL);
        const u32 *reg;
        int len, na = fdt_address_cells(fdt, 0), ns = fdt_size_cells(fdt, 0);
        u64 base = 0, size = 0;

        if (!name || strncmp(name, "memory", 6))
            continue;

        reg = fdt_getprop(fdt, node, "reg", &len);
        if (!reg || na < 1 || ns < 1 || len < (na + ns) * 4)
            continue;

        for (int i = 0; i < na; i++)
            base = (base << 32) | fdt32_to_cpu(reg[i]);
        for (int i = 0; i < ns; i++)
            size = (size << 32) | fdt32_to_cpu(reg[na + i]);

        if (!size)
            continue;

        /*
         * Prefer the bank our image lives in; otherwise the largest one.
         * ABL describes several reserved carveouts as separate banks.
         */
        if (gs201.load_base >= base && gs201.load_base < base + size) {
            gs201.ram_base = base;
            gs201.ram_size = size;
            return;
        }
        if (size > gs201.ram_size) {
            gs201.ram_base = base;
            gs201.ram_size = size;
        }
    }
}

static void fdt_parse_uart(const void *fdt)
{
    u64 base, size;

    if (!fdt_find_compatible(fdt, "arm,pl011", &base, &size)) {
        gs201.uart_base = base;
        gs201.uart_is_pl011 = true;
        return;
    }

    if (!fdt_find_compatible(fdt, "samsung,exynos-uart", &base, &size) ||
        !fdt_find_compatible(fdt, "google,gs201-uart", &base, &size)) {
        gs201.uart_base = base;
        gs201.uart_is_pl011 = false;
    }
}

static void fdt_parse_usb(const void *fdt)
{
    u64 base, size;

    if (!fdt_find_compatible(fdt, "synopsys,dwc3", &base, &size))
        gs201.usb_base = base;
}

static void setup_console(void)
{
    gs201_uart_init();

    if (gs201_video_init() < 0) {
        printf("gs201: no display handoff; console is UART only\n");
        return;
    }

    /* fb.c takes its geometry from cur_boot_args.video. */
    cur_boot_args.video.base = gs201.fb_base;
    cur_boot_args.video.display = 1;
    cur_boot_args.video.stride = gs201.fb_stride;
    cur_boot_args.video.width = gs201.fb_width;
    cur_boot_args.video.height = gs201.fb_height;
    /* 1080x2400 phone panel: flag retina so fb_init() picks the 16x32 font. */
    cur_boot_args.video.depth = gs201.fb_depth | FB_DEPTH_FLAG_RETINA;

    fb_init(true);
    fb_display_logo();
    fb_set_active(true);
}

void gs201_main(void *fdt, void *base)
{
    bool usb_ready;

    memset64(_bss_start, 0, _bss_end - _bss_start);

    gs201.fdt = fdt;
    gs201.load_base = (u64)base;
    gs201.ram_base = GS201_DRAM_BASE;
    gs201.ram_size = 0;
    gs201.uart_base = 0x10A00000;
    gs201.usb_base = GS201_USB_BASE;
    gs201.timebase = GS201_TIMEBASE;

    if (fdt && fdt_check_header(fdt) == 0) {
        fdt_parse_memory(fdt);
        fdt_parse_uart(fdt);
        fdt_parse_usb(fdt);
    }
    if (!gs201.ram_size) {
        gs201.ram_base = GS201_DRAM_BASE;
        gs201.ram_size = GS201_DRAM_SIZE;
    }

    /* An appended Mach-O payload sits right after our own image. */
    if (*(const u32 *)_payload_start == MACH_O_MAGIC)
        gs201.payload = _payload_start;

    /*
     * Keep the heap above the memory XNU will manage.  m1n1 itself (code,
     * exception vectors, stacks) stays below the 2 MiB gap under
     * XNU_LOAD_ADDR, so the kernel can hand its own region to the allocator
     * immediately without stepping on the shim that serves its hypercalls.
     */
    cur_boot_args.top_of_kernel_data = ALIGN_UP(GS201_XNU_LOAD_ADDR + GS201_XNU_MEM_SIZE, 0x200000);
    /* Publish synthetic boot args for the host proxy; GS201 has no Apple ADT. */
    cur_boot_args.revision = 2;
    cur_boot_args.version = 2;
    cur_boot_args.phys_base = gs201.ram_base;
    cur_boot_args.virt_base = gs201.ram_base;
    cur_boot_args.mem_size = gs201.ram_size;
    cur_boot_args.rv2.mem_size_actual = gs201.ram_size;
    boot_args_addr = (u64)&cur_boot_args;

    heapblock_init();
    heapblock_set_limit((void *)(gs201.ram_base + gs201.ram_size));

    exception_initialize();
    setup_console();

    printf("\n\nm1n1 %s (GS201/lynx)\n", m1n1_version);
    printf("Copyright The Asahi Linux Contributors\n");
    printf("Licensed under the MIT license\n\n");

    printf("Running in EL%lu\n", mrs(CurrentEL) >> 2);
    printf("Loaded at 0x%lx, DRAM 0x%lx+0x%lx\n", gs201.load_base, gs201.ram_base,
           gs201.ram_size);
    printf("Heap 0x%lx..0x%lx\n", cur_boot_args.top_of_kernel_data,
           gs201.ram_base + gs201.ram_size);
    if (gs201.fdt)
        printf("Firmware FDT at %p\n", gs201.fdt);
    printf("Initialization complete.\n");
    /*
     * Keep m1n1 in its USB proxy before chainloading an appended payload.
     * P_EXIT makes uartproxy_run() return, so the host explicitly chooses
     * when to hand off to XNU.
     *
     * QEMU has no controller at the GS201 DWC3 address.  Retain its
     * non-interactive payload boot path when the USB probe fails.
     */
    usb_ready = gs201_usb_init();
    if (usb_ready || !gs201.payload) {
        printf("Running proxy...\n");
        uartproxy_run(NULL);
    }

    if (gs201.payload) {
        printf("Found XNU payload at %p; booting\n", gs201.payload);
        gs201_xnu_boot(gs201.payload, NULL);
    }

    panic("Proxy returned with no payload!\n");

}
