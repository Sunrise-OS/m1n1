/* SPDX-License-Identifier: MIT */

/*
 * Google GS201 (Tensor G2) / Pixel 7a "lynx" platform layer.
 *
 * m1n1 is loaded by ABL as an Android boot image kernel and entered at EL2
 * with the bootloader's own FDT in x0.  There is no ADT, no iBoot boot_args
 * and no Apple hardware, so everything the generic code expects from the
 * Apple platform has to be synthesized here from the FDT and from the
 * hardware state ABL leaves behind (display pipeline, UART, watchdogs).
 */

#ifndef __GS201_H__
#define __GS201_H__

#include "types.h"

/* Watchdog blocks ABL leaves armed per CPU cluster. */
#define GS201_WDT_CLUSTER0 0x10060000
#define GS201_WDT_CLUSTER1 0x10070000

/* DECON scanout buffer ABL keeps live (1080x2400 XRGB8888). */
#define GS201_FB_BASE   0x86000000
#define GS201_FB_WIDTH  1080
#define GS201_FB_HEIGHT 2400
#define GS201_FB_STRIDE (GS201_FB_WIDTH * 4)

/* DWC3 register window. */
#define GS201_USB_BASE 0x11210000

/*
 * Where XNU is loaded.  For Apple arm64 kernels linked at __TEXT
 * 0xfffffe0007004000 with a 32 MiB block bootstrap mapping from DRAM base
 * 0x80000000, __TEXT must land at physical 0x81004000 so that:
 *   (link_va & ~0x1ffffff) maps to (phys_base & ~0x1ffffff) == 0x80000000,
 * and every segment's loaded physical address exactly matches its mapped PA.
 */
#define GS201_XNU_LOAD_ADDR 0x81004000ULL

/* Fallbacks if the FDT cannot be parsed. */
#define GS201_DRAM_BASE 0x80000000ULL
#define GS201_DRAM_SIZE 0x200000000ULL
#define GS201_TIMEBASE  24576000

struct gs201_platform {
    const void *fdt;

    u64 load_base; /* where ABL loaded us (only used for sanity checks) */
    u64 ram_base;
    u64 ram_size;
    void *payload; /* appended Mach-O, or NULL */

    bool video;
    u64 fb_base;
    u32 fb_stride;
    u32 fb_width, fb_height, fb_depth;

    u64 uart_base;
    bool uart_is_pl011;
    u32 uart_clock;

    u64 usb_base;

    u32 timebase;

    /*
     * The XNU image currently running as an EL1 guest.  A fault the guest
     * takes back to EL2 is reported against this, so the dump names the
     * kernel offset instead of an opaque KVA.
     */
    u64 guest_virt_base;
    u64 guest_phys_base;
    u64 guest_image_size;
};

extern struct gs201_platform gs201;

void gs201_main(void *fdt, void *base) __attribute__((noreturn));

/* True if the firmware FDT describes a node with the given compatible. */
bool gs201_fdt_compatible(const char *compatible);

/* uart.c */
int gs201_uart_init(void);

/* video.c */
int gs201_video_init(void);

/* exc.c */
void gs201_exc_init(void);

/* usb.c: returns true when the CDC-ACM proxy device is available. */
bool gs201_usb_init(void);

/* xnu.c: boots an appended XNU image; does not return. */
/* xnu.c: boots an XNU image (payload or proxy-uploaded); does not return. */
/* cmdline may be NULL to fall back to the firmware FDT bootargs. */
void gs201_xnu_boot(void *payload, const char *cmdline) __attribute__((noreturn));

/* afdt.c */
void *gs201_afdt_build(u32 *len_out);
u32 gs201_afdt_length(const void *afdt);

/* wdt.c */
void wdt_disable(void);
void wdt_reboot(void);

#endif
