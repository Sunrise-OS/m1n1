/* SPDX-License-Identifier: MIT */

#include "dart.h"
#include "adt.h"
#include "assert.h"
#include "devicetree.h"
#include "malloc.h"
#include "memory.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define DART_T8020_CONFIG      0x60
#define DART_T8020_CONFIG_LOCK BIT(15)

#define DART_T8020_ERROR              0x40
#define DART_T8020_ERROR_STREAM_SHIFT 24
#define DART_T8020_ERROR_STREAM_MASK  0xf
#define DART_T8020_ERROR_CODE_MASK    0xffffff
#define DART_T8020_ERROR_FLAG         BIT(31)
#define DART_T8020_ERROR_READ_FAULT   BIT(4)
#define DART_T8020_ERROR_WRITE_FAULT  BIT(3)
#define DART_T8020_ERROR_NO_PTE       BIT(2)
#define DART_T8020_ERROR_NO_PMD       BIT(1)
#define DART_T8020_ERROR_NO_TTBR      BIT(0)

#define DART_T8020_STREAM_SELECT 0x34

#define DART_T8020_STREAM_COMMAND            0x20
#define DART_T8020_STREAM_COMMAND_BUSY       BIT(2)
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)

#define DART_T8020_STREAM_COMMAND_BUSY_TIMEOUT 100

#define DART_T8020_STREAM_REMAP 0x80

#define DART_T8020_ERROR_ADDR_HI 0x54
#define DART_T8020_ERROR_ADDR_LO 0x50

#define DART_T8020_ENABLED_STREAMS 0xfc

#define DART_T8020_TCR_OFF              0x100
#define DART_T8020_TCR_TRANSLATE_ENABLE BIT(7)
#define DART_T8020_TCR_BYPASS_DART      BIT(8)
#define DART_T8020_TCR_BYPASS_DAPF      BIT(12)

#define DART_T8020_TTBR_OFF   0x200
#define DART_T8020_TTBR_VALID BIT(31)
#define DART_T8020_TTBR_ADDR  GENMASK(30, 0)
#define DART_T8020_TTBR_SHIFT 12

#define DART_PTE_OFFSET_SHIFT     14
#define DART_PTE_SP_START         GENMASK(63, 52)
#define DART_PTE_SP_END           GENMASK(51, 40)
#define DART_T8020_PTE_OFFSET     GENMASK(39, 14)
#define DART_T6000_PTE_OFFSET     GENMASK(39, 10)
#define DART_T8020_PTE_DISABLE_SP BIT(1)
#define DART_T6000_PTE_REALTIME   BIT(1)
#define DART_PTE_VALID            BIT(0)

#define DART_T8110_TTBR_OFF   0x1400
#define DART_T8110_TTBR_VALID BIT(0)
#define DART_T8110_TTBR_ADDR  GENMASK(29, 2)
#define DART_T8110_TTBR_SHIFT 14

#define DART_T8110_TCR_OFF              0x1000
#define DART_T8110_TCR_REMAP            GENMASK(11, 8)
#define DART_T8110_TCR_REMAP_EN         BIT(7)
#define DART_T8110_TCR_BYPASS_DAPF      BIT(2)
#define DART_T8110_TCR_BYPASS_DART      BIT(1)
#define DART_T8110_TCR_TRANSLATE_ENABLE BIT(0)

#define DART_T8110_TLB_CMD              0x80
#define DART_T8110_TLB_CMD_BUSY         BIT(31)
#define DART_T8110_TLB_CMD_OP           GENMASK(10, 8)
#define DART_T8110_TLB_CMD_OP_FLUSH_ALL 0
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
#define DART_T8110_TLB_CMD_STREAM       GENMASK(7, 0)

#define DART_T8110_PROTECT          0x200
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)

#define DART_T8110_ENABLE_STREAMS  0xc00
#define DART_T8110_DISABLE_STREAMS 0xc20

#define DART_MAX_TTBR_COUNT 4

#define DART_GEN3_PTE_OFFSET GENMASK(37, 10)
#define DART_GEN3_ROUTE      0x4604

#define DART_TCR(dart) (dart->regs + dart->params->tcr_off + 4 * dart->device)
#define DART_TTBR(dart, idx)                                                                       \
    (dart->regs + dart->params->ttbr_off + 4 * dart->params->ttbr_count * dart->device + 4 * idx)

struct dart_params {
    int sid_count;

    u64 pte_flags;
    u64 offset_mask;

    u64 tcr_enabled;
    u64 tcr_disabled;
    u64 tcr_off;

    u64 ttbr_valid;
    u64 ttbr_addr;
    u64 ttbr_shift;
    u64 ttbr_off;
    int ttbr_count;

    void (*tlb_invalidate)(dart_dev_t *dart);
};

struct dart_dev {
    bool locked;
    bool keep;
    uintptr_t regs;
    u8 device;
    enum dart_type_t type;
    const struct dart_params *params;
    u64 vm_base;
    uintptr_t tlbi_aperture;
    u8 tlbi_route;

    u64 *l1[DART_MAX_TTBR_COUNT];
};

static void dart_t8020_tlb_invalidate(dart_dev_t *dart)
{
    write32(dart->regs + DART_T8020_STREAM_SELECT, BIT(dart->device));

    /* ensure that the DART can see the updated pagetables before invalidating */
    dma_wmb();
    write32(dart->regs + DART_T8020_STREAM_COMMAND, DART_T8020_STREAM_COMMAND_INVALIDATE);

    if (poll32(dart->regs + DART_T8020_STREAM_COMMAND, DART_T8020_STREAM_COMMAND_BUSY, 0, 100))
        printf("dart: DART_T8020_STREAM_COMMAND_BUSY did not clear.\n");
}

static void dart_t8110_tlb_invalidate(dart_dev_t *dart)
{
    /* ensure that the DART can see the updated pagetables before invalidating */
    dma_wmb();
    write32(dart->regs + DART_T8110_TLB_CMD,
            FIELD_PREP(DART_T8110_TLB_CMD_OP, DART_T8110_TLB_CMD_OP_FLUSH_SID) |
                FIELD_PREP(DART_T8110_TLB_CMD_STREAM, dart->device));

    if (poll32(dart->regs + DART_T8110_TLB_CMD, DART_T8110_TLB_CMD_BUSY, 0, 100))
        printf("dart: DART_T8110_TLB_CMD_BUSY did not clear.\n");
}

/* 26A428 SPTM gen3dart_write_tlbi_cmd_payload: a 128-bit aperture write,
 * followed by VALE1OS, broadcasts a SID invalidation. The old +0x80 command
 * register is used for translation queries, not this invalidation path.
 * DCP SID 23 accepted this sequence on J873g without an exception. */
static void dart_gen3_tlb_invalidate(dart_dev_t *dart)
{
    u64 lo = BIT(60) | ((u64)dart->tlbi_route << 56) | ((u64)dart->device << 48);
    u64 hi = 0;
    dma_wmb();
    asm volatile("stp %0, %1, [%2]\n"
                 "dsb ish\n"
                 ".inst 0xd50881bf\n" /* tlbi vale1os, xzr */
                 "dsb osh\n"
                 : : "r"(lo), "r"(hi), "r"(dart->tlbi_aperture) : "memory");
}

const struct dart_params dart_t8020 = {
    .sid_count = 32,
    .pte_flags = FIELD_PREP(DART_PTE_SP_END, 0xfff) | FIELD_PREP(DART_PTE_SP_START, 0) |
                 DART_T8020_PTE_DISABLE_SP | DART_PTE_VALID,
    .offset_mask = DART_T8020_PTE_OFFSET,
    .tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
    .tcr_disabled = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,
    .tcr_off = DART_T8020_TCR_OFF,
    .ttbr_valid = DART_T8020_TTBR_VALID,
    .ttbr_addr = DART_T8020_TTBR_ADDR,
    .ttbr_shift = DART_T8020_TTBR_SHIFT,
    .ttbr_off = DART_T8020_TTBR_OFF,
    .ttbr_count = 4,
    .tlb_invalidate = dart_t8020_tlb_invalidate,
};

const struct dart_params dart_t6000 = {
    .sid_count = 32,
    .pte_flags =
        FIELD_PREP(DART_PTE_SP_END, 0xfff) | FIELD_PREP(DART_PTE_SP_START, 0) | DART_PTE_VALID,
    .offset_mask = DART_T6000_PTE_OFFSET,
    .tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
    .tcr_disabled = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,
    .tcr_off = DART_T8020_TCR_OFF,
    .ttbr_valid = DART_T8020_TTBR_VALID,
    .ttbr_addr = DART_T8020_TTBR_ADDR,
    .ttbr_shift = DART_T8020_TTBR_SHIFT,
    .ttbr_off = DART_T8020_TTBR_OFF,
    .ttbr_count = 4,
    .tlb_invalidate = dart_t8020_tlb_invalidate,
};

const struct dart_params dart_t8110 = {
    .sid_count = 256,
    .pte_flags =
        FIELD_PREP(DART_PTE_SP_END, 0xfff) | FIELD_PREP(DART_PTE_SP_START, 0) | DART_PTE_VALID,
    .offset_mask = DART_T6000_PTE_OFFSET,
    .tcr_enabled = DART_T8110_TCR_TRANSLATE_ENABLE,
    .tcr_disabled = DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART,
    .tcr_off = DART_T8110_TCR_OFF,
    .ttbr_valid = DART_T8110_TTBR_VALID,
    .ttbr_addr = DART_T8110_TTBR_ADDR,
    .ttbr_shift = DART_T8110_TTBR_SHIFT,
    .ttbr_off = DART_T8110_TTBR_OFF,
    .ttbr_count = 1,
    .tlb_invalidate = dart_t8110_tlb_invalidate,
};

/* The two-level Gen3 stream format was checked against live DCP firmware
 * mappings. Three-level streams, cold initialization and PMGR2 are separate
 * work; never substitute the older driver for those cases. */
static const struct dart_params dart_gen3 = {
    .sid_count = 256,
    .pte_flags = FIELD_PREP(DART_PTE_SP_END, 0xfff) | DART_PTE_VALID,
    .offset_mask = DART_GEN3_PTE_OFFSET,
    .tcr_enabled = DART_T8110_TCR_TRANSLATE_ENABLE,
    .tcr_disabled = DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART,
    .tcr_off = DART_T8110_TCR_OFF,
    .ttbr_valid = DART_T8110_TTBR_VALID,
    .ttbr_addr = DART_T8110_TTBR_ADDR,
    .ttbr_shift = DART_T8110_TTBR_SHIFT,
    .ttbr_off = DART_T8110_TTBR_OFF,
    .ttbr_count = 1,
    .tlb_invalidate = dart_gen3_tlb_invalidate,
};

static dart_dev_t *dart_gen3_adopt_adt(int node, uintptr_t base, int device, bool keep_pts)
{
    /* Require an iBoot-provisioned display stream. In particular, do not read
     * an unpowered USB DART while PMGR2 service control is unavailable. */
    if (!keep_pts || device < 0 || device >= dart_gen3.sid_count) {
        printf("dart: Gen3 cold initialization requires PMGR2 support\n");
        return NULL;
    }

    int tlbi_path[8];
    int tlbi_node = adt_path_offset_trace(adt, "/arm-io/dart-tlbi-aperture", tlbi_path);
    u64 aperture, size, offset;
    u32 sid_count;
    if (tlbi_node < 0 ||
        adt_get_reg(adt, tlbi_path, "reg", 0, &aperture, &size) < 0 ||
        ADT_GETPROP(adt, node, "tlbi-aperture-offset", &offset) < 0 ||
        ADT_GETPROP(adt, node, "sid-count", &sid_count) < 0 ||
        sid_count > 256 || (u32)device >= sid_count || size < 16 ||
        offset > size - 16 || (offset & 15)) {
        printf("dart: invalid Gen3 SID/TLBI metadata\n");
        return NULL;
    }

    u32 config = read32(base);
    u32 version = read32(base + 8) & 0xffff;
    u32 sid_config = read32(base + DART_T8110_TCR_OFF + 4 * device);
    u32 enabled = read32(base + DART_T8110_ENABLE_STREAMS + 4 * (device >> 5));
    u32 ttbr = read32(base + DART_T8110_TTBR_OFF + 4 * device);
    if (((config >> 24) & 15) != 14 || version != 0x300 ||
        (sid_config & 15) != 1 || !(enabled & BIT(device & 31)) ||
        !(ttbr & DART_T8110_TTBR_VALID)) {
        printf("dart: Gen3 stream %d is not an active two-level 16K stream\n", device);
        return NULL;
    }

    dart_dev_t *dart = calloc(1, sizeof(*dart));
    if (!dart)
        return NULL;
    dart->regs = base;
    dart->device = device;
    dart->type = DART_GEN3;
    dart->params = &dart_gen3;
    dart->keep = true;
    dart->locked = !!(read32(base + 0x200) & read32(base + 0x208) & 1);
    dart->l1[0] = (u64 *)(FIELD_GET(DART_T8110_TTBR_ADDR, ttbr) << DART_T8110_TTBR_SHIFT);
    dart->tlbi_aperture = aperture + offset;
    dart->tlbi_route = read32(base + DART_GEN3_ROUTE) & 15;
    /* Table indices use the low 36 bits, but firmware buffer grants must
     * retain the ADT's high IOVA window. T8152 DCP rejects low addresses as
     * MMIO (observed for an RTKit buffer grant at 0x10000000). */
    char prop[24];
    snprintf(prop, sizeof(prop), "vm-base-%d", device);
    if (ADT_GETPROP(adt, node, prop, &dart->vm_base) < 0)
        ADT_GETPROP(adt, node, "vm-base", &dart->vm_base);
    printf("dart: adopted Gen3 SID %d at 0x%lx, root %p, TLBI route %u%s\n", device,
           base, dart->l1[0], dart->tlbi_route, dart->locked ? " (locked)" : "");
    return dart;
}

dart_dev_t *dart_init(uintptr_t base, u8 device, bool keep_pts, enum dart_type_t type)
{
    if (type == DART_GEN3) {
        printf("dart: Gen3 requires ADT aperture and stream metadata\n");
        return NULL;
    }
    dart_dev_t *dart = calloc(1, sizeof(*dart));
    if (!dart)
        return NULL;

    dart->regs = base;
    dart->device = device;
    dart->type = type;

    switch (type) {
        case DART_T8020:
            dart->params = &dart_t8020;
            break;
        case DART_T8110:
            dart->params = &dart_t8110;
            break;
        case DART_T6000:
            dart->params = &dart_t6000;
            break;
        default:
            free(dart);
            return NULL;
    }

    if (device >= dart->params->sid_count) {
        printf("dart: device %d is too big for this DART type\n", device);
        free(dart);
        return NULL;
    }

    switch (type) {
        case DART_T8020:
        case DART_T6000:
            if (read32(dart->regs + DART_T8020_CONFIG) & DART_T8020_CONFIG_LOCK)
                dart->locked = true;
            set32(dart->regs + DART_T8020_ENABLED_STREAMS, BIT(device & 0x1f));
            break;
        case DART_T8110:
            if (read32(dart->regs + DART_T8110_PROTECT) & DART_T8110_PROTECT_TTBR_TCR)
                dart->locked = true;
            write32(dart->regs + DART_T8110_ENABLE_STREAMS + 4 * (device >> 5), BIT(device & 0x1f));
            break;
        default:
            break; /* Unsupported types were rejected above. */
    }

    dart->keep = keep_pts;

    if (dart->locked || keep_pts) {
        for (int i = 0; i < dart->params->ttbr_count; i++) {
            u32 ttbr = read32(DART_TTBR(dart, i));
            if (ttbr & dart->params->ttbr_valid)
                dart->l1[i] =
                    (u64 *)(FIELD_GET(dart->params->ttbr_addr, ttbr) << dart->params->ttbr_shift);
        }
    }

    for (int i = 0; i < dart->params->ttbr_count; i++) {
        if (dart->l1[i])
            continue;

        dart->l1[i] = memalign(SZ_16K, SZ_16K);
        if (!dart->l1[i])
            goto error;
        memset(dart->l1[i], 0, SZ_16K);

        write32(DART_TTBR(dart, i),
                dart->params->ttbr_valid |
                    FIELD_PREP(dart->params->ttbr_addr,
                               ((uintptr_t)dart->l1[i]) >> dart->params->ttbr_shift));
    }

    if (!dart->locked && !keep_pts)
        write32(DART_TCR(dart), dart->params->tcr_enabled);

    dart->params->tlb_invalidate(dart);
    return dart;

error:
    if (!dart->locked)
        free(dart->l1);
    free(dart);
    return NULL;
}

dart_dev_t *dart_init_adt(const char *path, int instance, int device, bool keep_pts)
{
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    if (node < 0) {
        printf("dart: Error getting DART node %s\n", path);
        return NULL;
    }

    u64 base;
    if (adt_get_reg(adt, dart_path, "reg", instance, &base, NULL) < 0) {
        printf("dart: Error getting DART %s base address.\n", path);
        return NULL;
    }

    enum dart_type_t type;
    const char *type_s;

    if (adt_is_compatible(adt, node, "dart,gen3")) {
        return dart_gen3_adopt_adt(node, base, device, keep_pts);
    } else if (adt_is_compatible(adt, node, "dart,t8020")) {
        type = DART_T8020;
        type_s = "t8020";
    } else if (adt_is_compatible(adt, node, "dart,t6000")) {
        type = DART_T6000;
        type_s = "t6000";
    } else if (adt_is_compatible(adt, node, "dart,t8110")) {
        type = DART_T8110;
        type_s = "t8110";
    } else {
        printf("dart: dart %s at 0x%lx is of an unknown type\n", path, base);
        return NULL;
    }

    dart_dev_t *dart = dart_init(base, device, keep_pts, type);

    if (!dart)
        return NULL;

    printf("dart: dart %s at 0x%lx is a %s%s\n", path, base, type_s,
           dart->locked ? " (locked)" : "");

    if (adt_getprop(adt, node, "real-time", NULL)) {
        for (int i = 0; i < dart->params->ttbr_count; i++) {
            printf("dart: dart %s.%d.%d L1 %d is real-time at %p\n", path, instance, device, i,
                   dart->l1[i]);
        }
    }
    u32 len;
    const void *prop = adt_getprop(adt, node, "vm-base", &len);
    if (prop) {
        if (len == sizeof(u32)) {
            u32 tmp;
            memcpy(&tmp, prop, sizeof(tmp));
            dart->vm_base = tmp;
        } else if (len == sizeof(u64)) {
            u64 tmp;
            memcpy(&tmp, prop, sizeof(tmp));
            dart->vm_base = tmp;
        } else {
            printf("dart: unexpected length of vm-base property: %u\n", len);
        }
    }
    if (dart->locked)
        dart->vm_base &= (1LLU << 36) - 1;

    return dart;
}

void dart_lock_adt(const char *path, int instance)
{
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    if (node < 0) {
        printf("dart: Error getting DART node %s\n", path);
        return;
    }

    u64 base;
    if (adt_get_reg(adt, dart_path, "reg", instance, &base, NULL) < 0) {
        printf("dart: Error getting DART %s base address.\n", path);
        return;
    }

    if (adt_is_compatible(adt, node, "dart,t8020") || adt_is_compatible(adt, node, "dart,t6000")) {
        if (!(read32(base + DART_T8020_CONFIG) & DART_T8020_CONFIG_LOCK))
            set32(base + DART_T8020_CONFIG, DART_T8020_CONFIG_LOCK);
    } else if (adt_is_compatible(adt, node, "dart,t8110")) {
        if (!(read32(base + DART_T8110_PROTECT) & DART_T8110_PROTECT_TTBR_TCR))
            set32(base + DART_T8110_PROTECT, DART_T8110_PROTECT_TTBR_TCR);
    } else {
        printf("dart: dart %s at 0x%lx is of an unknown type\n", path, base);
    }
}

dart_dev_t *dart_init_fdt(void *dt, u32 phandle, int device, bool keep_pts)
{
    int node = fdt_node_offset_by_phandle(dt, phandle);
    if (node < 0) {
        printf("FDT: node for phandle %u not found\n", phandle);
        return NULL;
    }

    u64 base = dt_get_address(dt, node);
    if (!base)
        return NULL;

    enum dart_type_t type;
    const char *type_s;
    const char *name = fdt_get_name(dt, node, NULL);

    if (fdt_node_check_compatible(dt, node, "apple,t8103-dart") == 0) {
        type = DART_T8020;
        type_s = "t8020";
    } else if (fdt_node_check_compatible(dt, node, "apple,t6000-dart") == 0) {
        type = DART_T6000;
        type_s = "t6000";
    } else if (fdt_node_check_compatible(dt, node, "apple,t8110-dart") == 0) {
        type = DART_T8110;
        type_s = "t8110";
    } else {
        printf("dart: dart %s at 0x%lx is of an unknown type\n", name, base);
        return NULL;
    }

    dart_dev_t *dart = dart_init(base, device, keep_pts, type);

    if (!dart)
        return NULL;

    printf("dart: dart %s at 0x%lx is a %s%s\n", name, base, type_s,
           dart->locked ? " (locked)" : "");

    return dart;
}

int dart_setup_pt_region(dart_dev_t *dart, const char *path, int device, u64 vm_base)
{
    int node = adt_path_offset(adt, path);
    if (node < 0) {
        printf("dart: Error getting DART node %s\n", path);
        return -1;
    }
    char pt_region_str[24];
    snprintf(pt_region_str, sizeof(pt_region_str), "pt-region-%d", device);
    char l2_tt_str[24];
    snprintf(l2_tt_str, sizeof(l2_tt_str), "l2-tt-%d", device);

    const struct adt_property *pt_region = adt_get_property(adt, node, pt_region_str);
    if (pt_region && pt_region->size == 16) {
        u64 region[2];
        memcpy(region, pt_region->value, sizeof(region));
        u64 tbl_count = (region[1] - region[0]) / SZ_16K;
        if (tbl_count > 64) {
            printf("dart: dart %s ignoring large %s, %lu L2 tables\n", path, pt_region_str,
                   tbl_count);
            return -1;
        }
        /* first index may or may not be the l1 table? */
        u64 l2_free = region[0] + SZ_16K;
        u64 l2_free_end = region[1];

        /* find the lowest unused L2 PT address */
        u64 vmstart = vm_base >> (14 + 11);
        int ttbr = (vmstart >> 11) & 3;
        for (u64 index = 0; index < 2048; index++) {
            if (!(dart->l1[ttbr][index] & DART_PTE_VALID))
                continue;
            u64 off = FIELD_GET(dart->params->offset_mask, dart->l1[ttbr][index])
                      << DART_PTE_OFFSET_SHIFT;
            if (off >= l2_free && off < l2_free_end)
                l2_free = off + SZ_16K;
        }

        /* ensure the first 2 L2 tables are initialized */
        tbl_count = min(2, tbl_count - 1);
        for (u64 index = 0; index < tbl_count; index++) {
            int ttbr = ((vmstart + index) >> 11) & 3;
            int idx = (vmstart + index) & 0x7ff;

            if (dart->l1[ttbr][idx] & DART_PTE_VALID) {
                /* m1n1 bug fixup: old versions used to clobber PTs */

                for (int j = 0; j < 2048; j++) {
                    if (j != idx && dart->l1[ttbr][j] == dart->l1[ttbr][idx]) {
                        printf("dart: clearing clobbered L1 PTE at %d, %d\n", ttbr, idx);
                        dart->l1[ttbr][idx] = 0;
                        break;
                    }
                }

                continue;
            }

            printf("dart: allocating L2 tbl at %d, %d to 0x%lx\n", ttbr, idx, l2_free);

            if (l2_free >= l2_free_end) {
                printf("dart: out of prealloc page tables\n");
                return -1;
            }

            memset((void *)l2_free, 0, SZ_16K);

            u64 offset = FIELD_PREP(dart->params->offset_mask, l2_free >> DART_PTE_OFFSET_SHIFT);
            dart->l1[ttbr][idx] = offset | DART_PTE_VALID;
            l2_free += SZ_16K;
        }

        u64 l2_tt[2] = {region[0], 2};
        int ret = adt_setprop(adt, node, l2_tt_str, &l2_tt, sizeof(l2_tt));
        if (ret < 0) {
            printf("dart: failed to update '%s/%s'\n", path, l2_tt_str);
        }

        dart->params->tlb_invalidate(dart);
    }

    return 0;
}

static u64 *dart_get_l2(dart_dev_t *dart, u32 idx)
{
    int ttbr = idx >> 11;
    idx &= 0x7ff;

    if (ttbr >= dart->params->ttbr_count || !dart->l1[ttbr])
        return NULL;

    if (dart->l1[ttbr][idx] & DART_PTE_VALID) {
        u64 off = FIELD_GET(dart->params->offset_mask, dart->l1[ttbr][idx])
                  << DART_PTE_OFFSET_SHIFT;
        return (u64 *)off;
    }

    u64 *tbl = memalign(SZ_16K, SZ_16K);
    if (!tbl)
        return NULL;

    memset(tbl, 0, SZ_16K);

    u64 offset = FIELD_PREP(dart->params->offset_mask, ((u64)tbl) >> DART_PTE_OFFSET_SHIFT);

    dart->l1[ttbr][idx] = offset | DART_PTE_VALID;

    return tbl;
}

static int dart_map_page(dart_dev_t *dart, uintptr_t iova, uintptr_t paddr, u32 flags)
{
    u32 l1_index = (iova >> 25) & 0x1fff;
    u32 l2_index = (iova >> 14) & 0x7ff;

    u64 *l2 = dart_get_l2(dart, l1_index);
    if (!l2) {
        printf("dart: couldn't create l2 for iova %lx\n", iova);
        return -1;
    }

    if (l2[l2_index] & DART_PTE_VALID) {
        printf("dart: iova %lx already has a valid PTE: %lx\n", iova, l2[l2_index]);
        return -1;
    }

    u64 offset = FIELD_PREP(dart->params->offset_mask, paddr >> DART_PTE_OFFSET_SHIFT);

    l2[l2_index] = offset | dart->params->pte_flags | flags;

    return 0;
}

int dart_map_flags(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len, u32 flags)
{
    uintptr_t paddr = (uintptr_t)bfr;
    u64 offset = 0;

    /* No IOMMU: the device sees physical addresses directly. */
    if (!dart)
        return 0;

    if (len % SZ_16K)
        return -1;
    if (paddr % SZ_16K)
        return -1;
    if (iova % SZ_16K)
        return -1;

    while (offset < len) {
        int ret = dart_map_page(dart, iova + offset, paddr + offset, flags);

        if (ret) {
            dart_unmap(dart, iova, offset);
            return ret;
        }

        offset += SZ_16K;
    }

    dart->params->tlb_invalidate(dart);
    return 0;
}

int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len)
{
    return dart_map_flags(dart, iova, bfr, len, 0);
}

static void dart_unmap_page(dart_dev_t *dart, uintptr_t iova)
{
    u32 ttbr = (iova >> 36) & 0x3;
    u32 l1_index = (iova >> 25) & 0x7ff;
    u32 l2_index = (iova >> 14) & 0x7ff;

    if (!(dart->l1[ttbr][l1_index] & DART_PTE_VALID))
        return;

    u64 *l2 = dart_get_l2(dart, l1_index);
    l2[l2_index] = 0;
}

void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len)
{
    if (!dart)
        return;

    if (len % SZ_16K)
        return;
    if (iova % SZ_16K)
        return;

    while (len) {
        dart_unmap_page(dart, iova);

        len -= SZ_16K;
        iova += SZ_16K;
    }

    dart->params->tlb_invalidate(dart);
}

void dart_free_l2(dart_dev_t *dart, uintptr_t iova)
{
    if (iova & ((1 << 25) - 1)) {
        printf("dart: %08lx is not at the start of L2 table\n", iova);
        return;
    }

    u32 ttbr = (iova >> 36) & 0x3;
    u32 l1_index = (iova >> 25) & 0x7ff;

    if (!(dart->l1[ttbr][l1_index] & DART_PTE_VALID))
        return;

    u64 *l2 = dart_get_l2(dart, l1_index);

    for (u32 idx = 0; idx < 2048; idx++) {
        if (l2[idx] & DART_PTE_VALID) {
            printf("dart: %08lx is still mapped\n", iova + (idx << 14));
            return;
        }
    }
    dart->l1[ttbr][l1_index] = 0;
    free(l2);
}

static void *dart_translate_internal(dart_dev_t *dart, uintptr_t iova, int silent)
{
    u32 ttbr = (iova >> 36) & 0x3;
    u32 l1_index = (iova >> 25) & 0x7ff;

    if ((int)ttbr >= dart->params->ttbr_count) {
        printf("dart[%lx %u]: ttbr out of range: %d\n", dart->regs, dart->device, ttbr);
        return NULL;
    }

    if (!dart->l1[ttbr]) {
        printf("dart[%lx %u]: l1[%u] is not set\n", dart->regs, dart->device, ttbr);
        return NULL;
    }

    if (!(dart->l1[ttbr][l1_index] & DART_PTE_VALID)) {
        if (!silent)
            printf("dart[%lx %u]: l1 translation failure %x %lx\n", dart->regs, dart->device,
                   l1_index, iova);
        return NULL;
    }

    u32 l2_index = (iova >> 14) & 0x7ff;
    u64 *l2 = (u64 *)(FIELD_GET(dart->params->offset_mask, dart->l1[ttbr][l1_index])
                      << DART_PTE_OFFSET_SHIFT);

    if (!(l2[l2_index] & DART_PTE_VALID)) {
        if (!silent)
            printf("dart[%lx %u]: l2 translation failure %x:%x %lx\n", dart->regs, dart->device,
                   l1_index, l2_index, iova);
        return NULL;
    }

    u32 offset = iova & 0x3fff;
    void *base =
        (void *)(FIELD_GET(dart->params->offset_mask, l2[l2_index]) << DART_PTE_OFFSET_SHIFT);

    return base + offset;
}

void *dart_translate(dart_dev_t *dart, uintptr_t iova)
{
    return dart_translate_internal(dart, iova, 0);
}

void *dart_translate_silent(dart_dev_t *dart, uintptr_t iova)
{
    return dart_translate_internal(dart, iova, 1);
}

u64 dart_search(dart_dev_t *dart, void *paddr)
{
    for (int ttbr = 0; ttbr < dart->params->ttbr_count; ++ttbr) {
        if (!dart->l1[ttbr])
            continue;
        for (u32 l1_index = 0; l1_index < 0x7ff; l1_index++) {
            if (!(dart->l1[ttbr][l1_index] & DART_PTE_VALID))
                continue;

            u64 *l2 = (u64 *)(FIELD_GET(dart->params->offset_mask, dart->l1[ttbr][l1_index])
                              << DART_PTE_OFFSET_SHIFT);
            for (u32 l2_index = 0; l2_index < 0x7ff; l2_index++) {
                if (!(l2[l2_index] & DART_PTE_VALID))
                    continue;
                u64 *dst = (u64 *)(FIELD_GET(dart->params->offset_mask, l2[l2_index])
                                   << DART_PTE_OFFSET_SHIFT);
                if (dst == paddr) {
                    u64 iova = ((u64)ttbr << 36) | ((u64)l1_index << 25) | (l2_index << 14);
                    if (dart->type == DART_GEN3)
                        iova |= dart->vm_base & ~GENMASK(35, 0);
                    return iova;
                }
            }
        }
    }

    return DART_PTR_ERR;
}

u64 dart_find_iova(dart_dev_t *dart, s64 start, size_t len)
{
    if (len % SZ_16K)
        return -1;
    if (start < 0 || start % SZ_16K)
        return -1;

    /* High ADT windows still cover a 36-bit page-table address space. */
    uintptr_t end = dart->vm_base + (1LLU << 36);
    uintptr_t iova = start;

    while (iova + len <= end) {

        if (dart_translate_internal(dart, iova, 1) == NULL) {
            size_t size;
            for (size = SZ_16K; size < len; size += SZ_16K) {
                if (dart_translate_internal(dart, iova + size, 1) != NULL)
                    break;
            }
            if (size == len)
                return iova;

            iova += size + SZ_16K;
        } else
            iova += SZ_16K;
    }

    return DART_PTR_ERR;
}

void dart_shutdown(dart_dev_t *dart)
{
    if (!dart->locked && !dart->keep)
        write32(DART_TCR(dart), dart->params->tcr_disabled);

    for (int i = 0; i < dart->params->ttbr_count; ++i)
        if (is_heap(dart->l1[i]))
            write32(DART_TTBR(dart, i), 0);

    for (int ttbr = 0; ttbr < dart->params->ttbr_count; ++ttbr) {
        for (int i = 0; i < SZ_16K / 8; ++i) {
            if (dart->l1[ttbr][i] & DART_PTE_VALID) {
                void *l2 = dart_get_l2(dart, i);
                if (is_heap(l2)) {
                    free(l2);
                    dart->l1[ttbr][i] = 0;
                }
            }
        }
    }

    dart->params->tlb_invalidate(dart);

    for (int i = 0; i < dart->params->ttbr_count; ++i)
        if (is_heap(dart->l1[i]))
            free(dart->l1[i]);
    free(dart);
}

u64 dart_vm_base(dart_dev_t *dart)
{
    return dart->vm_base;
}
