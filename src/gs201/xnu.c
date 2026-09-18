/* SPDX-License-Identifier: MIT */

/*
 * Boot XNU on GS201/lynx.
 *
 * This mirrors the handoff the U-Boot port proved out on this board:
 *
 *  - load the Mach-O (a DEVELOPMENT_ARM64_VMAPPLE build) at 0x80204000,
 *  - describe it with struct xnu_boot_arguments and an AFDT,
 *  - leave EL2 with the HVC shim installed (our exception vectors answer the
 *    VMAPPLE CPU-service hypercalls; see exc.c),
 *  - drop to EL1 with MMU/caches off, PAC executing natively and the physical
 *    counter available.
 *
 * There is no Apple hypervisor and no iBoot, so nothing here is optional: a
 * VMAPPLE kernel spins on a failed PAC setter and never reaches its console.
 */

#include "gs201.h"

#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define MACH_O_MAGIC 0xfeedfacf

#define LOAD_COMMAND_SEGMENT        0x19
#define LOAD_COMMAND_UNIXTHREAD     0x5
#define LOAD_COMMAND_MAIN           0x80000028
#define LOAD_COMMAND_CHAINED_FIXUPS 0x80000034
#define MACH_O_EXEC                 0x2
#define MACH_O_FILESET              0xc

/*
 * A `config_sptm` build (e.g. the vphone600 research kernelcache) is entered
 * by Apple's Secure Page Table Monitor at GL2 -- x0 = SPTM_CPU_* sentinel,
 * x1 = boot args, x2 = sptm_bootstrap_args_xnu_t *, EL1 already translated --
 * and its entry distinguishes a monitor-triggered panic from a cold boot,
 * which is what identifies the build.
 */
#define SPTM_ENTRY_MOV_X8_4  0xd2800088u /* mov x8, #SPTM_CPU_PANIC */
#define SPTM_ENTRY_CMP_X0_X8 0xeb08001fu /* cmp x0, x8 */

#define XNU_CMDLINE_LEN 1024 /* modern XNU BOOT_LINE_LENGTH */

struct mach_o_header {
    u32 magic;
    u32 cpu_type;
    u32 cpu_subtype;
    u32 file_type;
    u32 commands_nb;
    u32 commands_len;
    u32 flags;
    u32 reserved;
};

struct mach_o_load_command {
    u32 command;
    u32 command_size;
};

struct linkedit_data_command {
    struct mach_o_load_command load_command;
    u32 dataoff;
    u32 datasize;
};

struct dyld_chained_fixups_header {
    u32 fixups_version;
    u32 starts_offset;
    u32 imports_offset;
    u32 symbols_offset;
    u32 imports_count;
    u32 imports_format;
    u32 symbols_format;
};

struct dyld_chained_starts_in_image {
    u32 seg_count;
    u32 seg_info_offset[1];
};

struct dyld_chained_starts_in_segment {
    u32 size;
    u16 page_size;
    u16 pointer_format;
    u64 segment_offset;
    u32 max_valid_pointer;
    u16 page_count;
    u16 page_start[1];
};

struct mach_o_segment_command {
    struct mach_o_load_command load_command;
    char segment_name[16];
    u64 dst;
    u64 dst_len;
    u64 src_offset;
    u64 src_len;
    u32 max_protection;
    u32 initial_protection;
    u32 sections_nb;
    u32 flags;
};

struct entry_point_command {
    struct mach_o_load_command load_command;
    u64 entryoff;
    u64 stacksize;
};

struct thread_command {
    struct mach_o_load_command load_command;
    u32 flavor;
    u32 count;
    struct {
        u64 x[29];
        u64 fp;
        u64 lr;
        u64 sp;
        u64 pc;
        u32 cpsr;
        u32 flags;
    } state;
};

struct xnu_video_information {
    u64 base_addr;
    u64 display;
    u64 bytes_per_row;
    u64 width;
    u64 height;
    u64 depth;
};

struct xnu_boot_arguments {
    u16 revision;
    u16 version;
    u64 virt_base;
    u64 phys_base;
    u64 mem_size;
    u64 phys_end;
    struct xnu_video_information video_information;
    u32 machine_type;
    u64 afdt;
    u32 afdt_length;
    char command_line[XNU_CMDLINE_LEN];
    u64 boot_flags;
    u64 mem_size_actual;
};

struct mach_o_load_info {
    u64 base;
    u64 entry;
    u64 end;
    u64 file_size;
    u64 entry_offset;
};

static u8 el1_stack[0x4000] ALIGNED(16);

/* 1 GiB: start_first_cpu's bootstrap page tables cannot cover more. */
#define XNU_BOOT_MEM_SIZE 0x40000000ULL

static int mach_o_scan(const void *image, struct mach_o_load_info *out)
{
    const struct mach_o_header *h = image;
    const struct mach_o_load_command *lc;
    u32 i;

    if (h->magic != MACH_O_MAGIC)
        return -1;
    if (h->file_type != MACH_O_EXEC && h->file_type != MACH_O_FILESET)
        return -1;
    if (!h->commands_nb || h->commands_nb > 4096 || h->commands_len > 0x100000)
        return -1;

    out->base = ~0ULL;
    out->end = 0;
    out->entry = 0;
    out->entry_offset = 0;
    out->file_size = sizeof(*h) + h->commands_len;

    lc = (const struct mach_o_load_command *)(h + 1);
    for (i = 0; i < h->commands_nb; i++) {
        if (lc->command_size < sizeof(*lc))
            return -1;

        if (lc->command == LOAD_COMMAND_SEGMENT) {
            const struct mach_o_segment_command *sc = (const struct mach_o_segment_command *)lc;

            if (!strncmp(sc->segment_name, "__PAGEZERO", 10) && !sc->src_len) {
                lc = (const void *)((const u8 *)lc + lc->command_size);
                continue;
            }

            if (sc->dst_len == 0 && sc->src_len == 0) {
                lc = (const void *)((const u8 *)lc + lc->command_size);
                continue;
            }
            if (sc->dst + sc->dst_len > out->end)
                out->end = sc->dst + sc->dst_len;
            if (sc->dst < out->base)
                out->base = sc->dst;
            if (sc->src_offset + sc->src_len > out->file_size)
                out->file_size = sc->src_offset + sc->src_len;
        }

        lc = (const void *)((const u8 *)lc + lc->command_size);
    }

    if (out->base == ~0ULL || out->end <= out->base)
        return -1;

    /* Second pass: entry point. */
    lc = (const struct mach_o_load_command *)(h + 1);
    for (i = 0; i < h->commands_nb; i++) {
        if (lc->command == LOAD_COMMAND_UNIXTHREAD) {
            out->entry = ((const struct thread_command *)lc)->state.pc;
        } else if (lc->command == LOAD_COMMAND_MAIN) {
            out->entry_offset = ((const struct entry_point_command *)lc)->entryoff;
        }
        lc = (const void *)((const u8 *)lc + lc->command_size);
    }

    if (out->entry_offset)
        out->entry = out->base + out->entry_offset;

    if (!out->entry)
        return -1;

    return 0;
}

static inline u64 read64_unaligned(u64 addr)
{
    u32 lo = read32(addr);
    u32 hi = read32(addr + 4);
    return (u64)lo | ((u64)hi << 32);
}

static inline void write64_unaligned(u64 addr, u64 val)
{
    write32(addr, (u32)val);
    write32(addr + 4, (u32)(val >> 32));
}

static void mach_o_apply_chained_fixups(const void *image, const struct mach_o_load_info *info)
{
    const struct mach_o_header *h = image;
    const struct mach_o_load_command *lc = (const struct mach_o_load_command *)(h + 1);
    const struct dyld_chained_fixups_header *hdr = NULL;

    for (u32 i = 0; i < h->commands_nb; i++) {
        if (lc->command == LOAD_COMMAND_CHAINED_FIXUPS) {
            const struct linkedit_data_command *ldc = (const struct linkedit_data_command *)lc;
            hdr = (const struct dyld_chained_fixups_header *)((const u8 *)image + ldc->dataoff);
            break;
        }
        lc = (const void *)((const u8 *)lc + lc->command_size);
    }

    if (!hdr || hdr->starts_offset == 0)
        return;

    const struct dyld_chained_starts_in_image *starts =
        (const struct dyld_chained_starts_in_image *)((const u8 *)hdr + hdr->starts_offset);

    u32 total_rebased = 0;

    for (u32 s = 0; s < starts->seg_count; s++) {
        u32 seg_off = starts->seg_info_offset[s];
        if (!seg_off)
            continue;

        const struct dyld_chained_starts_in_segment *seg_starts =
            (const struct dyld_chained_starts_in_segment *)((const u8 *)starts + seg_off);

        /* segment_offset is the VM offset from __TEXT (which is at info->base) */
        u64 seg_phys_base = GS201_XNU_LOAD_ADDR + seg_starts->segment_offset;

        for (u32 p = 0; p < seg_starts->page_count; p++) {
            u16 start = seg_starts->page_start[p];
            if (start == 0xffff)
                continue;

            u64 chain_loc = seg_phys_base + (u64)p * seg_starts->page_size + start;
            while (1) {
                u64 raw = read64_unaligned(chain_loc);
                u64 auth = (raw >> 63) & 1;
                u64 next = (raw >> 51) & 0x7ff;
                u64 target = raw & 0xffffffff;
                u64 high8 = (raw >> 32) & 0xff;
                u64 val;

                if (auth || high8 == 0)
                    val = info->base + target;
                else
                    val = (high8 << 56) | target;

                write64_unaligned(chain_loc, val);
                total_rebased++;

                if (next == 0)
                    break;
                chain_loc += next * 4;
            }
        }
    }

    printf("XNU: applied %u dyld chained fixups\n", total_rebased);
}

static void mach_o_load(const void *image, const struct mach_o_load_info *info)
{
    const struct mach_o_header *h = image;
    const struct mach_o_load_command *lc = (const struct mach_o_load_command *)(h + 1);

    for (u32 i = 0; i < h->commands_nb; i++) {
        if (lc->command == LOAD_COMMAND_SEGMENT) {
            const struct mach_o_segment_command *sc = (const struct mach_o_segment_command *)lc;

            if (sc->dst_len && (strncmp(sc->segment_name, "__PAGEZERO", 10) || sc->src_len)) {
                void *dst = (void *)(GS201_XNU_LOAD_ADDR + (sc->dst - info->base));

                if (sc->src_len)
                    memcpy(dst, (const u8 *)image + sc->src_offset, sc->src_len);
                if (sc->dst_len > sc->src_len)
                    memset((u8 *)dst + sc->src_len, 0, sc->dst_len - sc->src_len);
            }
        }
        lc = (const void *)((const u8 *)lc + lc->command_size);
    }

    mach_o_apply_chained_fixups(image, info);
}

static void get_command_line(char *out, size_t len, const char *override)
{
    const void *fdt = gs201.fdt;
    const char *bootargs = NULL;
    int chosen;

    out[0] = 0;

    if (override && override[0]) {
        strncpy(out, override, len - 1);
        out[len - 1] = 0;
        return;
    }

    if (!fdt || fdt_check_header(fdt))
        return;

    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0)
        return;

    bootargs = fdt_getprop(fdt, chosen, "bootargs", NULL);
    if (!bootargs)
        return;

    strncpy(out, bootargs, len - 1);
    out[len - 1] = 0;
}

/*
 * True if the physical entry point is a `config_sptm` build's prologue.
 */
static bool xnu_entry_is_sptm_build(u64 entry)
{
    const u32 *prologue = (const u32 *)entry;

    return prologue[0] == SPTM_ENTRY_MOV_X8_4 && prologue[1] == SPTM_ENTRY_CMP_X0_X8;
}

/*
 * Drop to EL1 and enter the kernel with the boot arguments in x0.
 *
 * U-Boot's armv8_switch_to_el1_m() is unusable here: it forces HCR_EL2.HCD and
 * may set APK/API, both of which break a VMAPPLE guest.  Start EL1 from a known
 * state instead and keep our vectors (and so the HVC shim) live at EL2.
 */
__attribute__((noreturn)) static void enter_el1(u64 boot_args, u64 entry)
{
    if (in_el2()) {
        msr(SCTLR_EL1, 0x30d00800ULL); /* MMU/D-cache/I-cache off, RES1 set */
        msr(CPACR_EL1, 3ULL << 20);    /* no FP/SIMD traps */

        u64 hcr = mrs(HCR_EL2);
        hcr |= BIT(31);                /* lower EL is AArch64 */
        hcr &= ~(BIT(29) | BIT(27));   /* HVC enabled, no TGE */
        /* Trap PAC instructions and key registers to EL2 shim */
        hcr &= ~(BIT(40) | BIT(41));
        msr(HCR_EL2, hcr);

        msr(SP_EL1, (u64)el1_stack + sizeof(el1_stack));
        msr(SPSR_EL2, 0x3c5); /* EL1h, DAIF masked */
        sysop("isb");
    }

    asm volatile("mov x0, %0\n"
                 "msr elr_el2, %1\n"
                 "eret\n" ::"r"(boot_args),
                 "r"(entry)
                 : "x0", "memory");

    __builtin_unreachable();
}

void gs201_xnu_boot(void *payload, const char *cmdline)
{
    struct mach_o_load_info info;
    struct xnu_boot_arguments *ba;
    const void *image = payload;
    u32 afdt_len;
    void *afdt;

    if (mach_o_scan(payload, &info) < 0)
        panic("XNU: payload is not a loadable Mach-O image\n");

    /*
     * XNU is loaded at 0x80204000, which an appended payload may straddle.
     * Relocate it out of the way (into the heap, above the kernel's managed
     * memory) before any segment is copied over the source.
     */
    u64 image_size = info.end - info.base;
    if ((u64)payload < GS201_XNU_LOAD_ADDR + image_size &&
        (u64)payload + info.file_size > GS201_XNU_LOAD_ADDR) {
        void *copy = malloc(info.file_size);

        if (!copy)
            panic("XNU: no room to relocate the payload\n");
        printf("XNU: relocating %lu KiB payload out of the load range\n", info.file_size >> 10);
        memcpy(copy, payload, info.file_size);
        image = copy;
        if (mach_o_scan(image, &info) < 0)
            panic("XNU: relocated payload is not a Mach-O image\n");
        image_size = info.end - info.base;
    }

    printf("XNU: base 0x%lx entry 0x%lx end 0x%lx\n", info.base, info.entry, info.end);

    mach_o_load(image, &info);

    u64 xnu_end = GS201_XNU_LOAD_ADDR + image_size;

    ba = (struct xnu_boot_arguments *)ALIGN_UP(xnu_end, 16);
    memset(ba, 0, sizeof(*ba));

    ba->revision = 2;
    ba->version = 2;
    ba->virt_base = info.base;
    ba->phys_base = GS201_XNU_LOAD_ADDR;

    u64 mem_size = gs201.ram_size - (GS201_XNU_LOAD_ADDR - gs201.ram_base);
    if (mem_size > XNU_BOOT_MEM_SIZE)
        mem_size = XNU_BOOT_MEM_SIZE;
    ba->mem_size = mem_size;
    ba->mem_size_actual = mem_size;
    ba->boot_flags = 0;
    ba->machine_type = 0;

    ba->video_information.base_addr = gs201.video ? gs201.fb_base : 0;
    ba->video_information.display = gs201.video ? 1 : 0;
    ba->video_information.bytes_per_row = gs201.video ? gs201.fb_stride : 0;
    ba->video_information.width = gs201.video ? gs201.fb_width : 0;
    ba->video_information.height = gs201.video ? gs201.fb_height : 0;
    ba->video_information.depth = gs201.video ? gs201.fb_depth : 0;

    get_command_line(ba->command_line, sizeof(ba->command_line), cmdline);
    if (ba->command_line[0])
        printf("XNU: cmdline: %s\n", ba->command_line);

    afdt = gs201_afdt_build(&afdt_len);
    if (afdt_len != gs201_afdt_length(afdt))
        panic("XNU: generated AFDT is malformed\n");

    void *afdt_dst = (void *)ALIGN_UP((u64)(ba + 1), 16);
    memcpy(afdt_dst, afdt, afdt_len);
    ba->afdt = (u64)afdt_dst;
    ba->afdt_length = afdt_len;
    ba->phys_end = ALIGN_UP((u64)afdt_dst + afdt_len, 0x10000);

    /*
     * The MMU is off at EL1, so enter at the image's physical address.
     * XNU's own start code builds its page tables from boot_args.virt_base.
     */
    u64 entry_phys = GS201_XNU_LOAD_ADDR + (info.entry - info.base);

    /*
     * A `config_sptm` build cannot be entered with the plain iBoot contract:
     * it is written to arrive at GL2, translated, with x0 = SPTM_CPU_* (0 for
     * a cold boot) and x2 = sptm_bootstrap_args_xnu_t *, then manages its page
     * tables by calling SPTM services.  Handing it boot_args in x0 sends it
     * down its monitor-panic path instead.
     */
    if (xnu_entry_is_sptm_build(entry_phys))
        panic("XNU: entry is a config_sptm build, which needs a Secure Page\n"
              "     Table Monitor stand-in (page tables at GL2 + SPTM service\n"
              "     dispatch); this platform provides neither.\n");

    printf("XNU: boot_args at %p, AFDT at %p (%u bytes), mem 0x%lx\n", ba, afdt_dst, afdt_len,
           mem_size);
    printf("XNU: entering at 0x%lx (phys; virt 0x%lx)\n", entry_phys, info.entry);

    /* So a fault the guest takes back to EL2 is reported against the image. */
    gs201.guest_virt_base = info.base;
    gs201.guest_phys_base = GS201_XNU_LOAD_ADDR;
    gs201.guest_image_size = image_size;

    enter_el1((u64)ba, entry_phys);
}
