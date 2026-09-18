/* SPDX-License-Identifier: MIT */

/*
 * Apple flattened device tree (AFDT) builder for the XNU handoff.
 *
 * XNU's early platform discovery uses SecureDT*() over a recursively encoded
 * tree that is *not* a Linux FDT.  A node is:
 *
 *     u32 property_count; u32 child_count;
 *     { char name[32]; u32 length; u8 value[length] padded to 4; } * properties
 *     child nodes, recursively
 *
 * Integers are native little-endian because XNU dereferences them directly.
 * The tree below is the minimum the VMAPPLE kernel needs to reach the console:
 * a root, /chosen (with dram-base/size so is_dram_addr() works), /defaults and
 * /cpus/cpu0.  Values that depend on the platform are passed in.
 */

#include "gs201.h"

#include "string.h"
#include "types.h"
#include "utils.h"

#define AFDT_MAX_SIZE 4096

struct afdt_prop {
    const char *name;
    const void *value;
    u32 len;
};

#define STR(s) (s), (sizeof(s))

static u8 afdt_data[AFDT_MAX_SIZE];
static u32 afdt_size;

static void raw(const void *p, u32 n)
{
    if (afdt_size + n > AFDT_MAX_SIZE)
        panic("AFDT: buffer overflow\n");

    memcpy(afdt_data + afdt_size, p, n);
    afdt_size += n;
}

static void put32(u32 v)
{
    raw(&v, 4);
}

static void pad4(void)
{
    while (afdt_size & 3) {
        u8 z = 0;
        raw(&z, 1);
    }
}

static void node_hdr(u32 nprops, u32 nchildren)
{
    put32(nprops);
    put32(nchildren);
}

static void props(const struct afdt_prop *p, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        char name[32] = {0};
        size_t l = strlen(p[i].name);

        if (l > sizeof(name) - 1)
            l = sizeof(name) - 1;
        memcpy(name, p[i].name, l);

        raw(name, sizeof(name));
        put32(p[i].len);
        raw(p[i].value, p[i].len);
        pad4();
    }
}

/*
 * Emit the whole tree.  uint32_t size is verified by the XNU-side walker
 * (afdt_length()), so anything malformed here would be rejected at handoff.
 */
void *gs201_afdt_build(u32 *out_len)
{
    u8 seed[32];
    u64 dram_base = gs201.ram_base;
    u64 dram_size = gs201.ram_size;
    u32 timebase = gs201.timebase;
    u32 enabled = 1, zero = 0;

    for (unsigned int i = 0; i < sizeof(seed); i++)
        seed[i] = 0x5a + i;

    /*
     * Keep the advertised DRAM within what the kernel is actually told about
     * (mem_size is capped at 1 GiB for the bootstrap page tables), so that
     * is_dram_addr() never disagrees with the allocator.
     */
    if (dram_size > 0x40000000ULL)
        dram_size = 0x40000000ULL;

    afdt_size = 0;

    /*
     * GS201 peripheral window and GIC.  pe_arm_get_soc_base_phys() takes
     * ranges[1] (u64) as the SoC base; pe_init_fiq() maps soc_base + each
     * GIC offset below.  Values are SoC constants from gs201.dtsi:
     * GICD 0x10400000 (64 KiB), GICR 0x10440000 (1 MiB).
     */
    static const u64 arm_io_ranges[2] = {0, 0x10000000};
    static const u64 gic_reg[4] = {0x400000, 0x10000, 0x440000, 0x100000};

    /* device-tree: 2 properties, 4 children (chosen, defaults, cpus, arm-io) */
    node_hdr(2, 4);
    {
        const struct afdt_prop p[] = {
            {"name", STR("device-tree")},
            {"target-type", STR("J718AP")},
        };
        props(p, ARRAY_SIZE(p));
    }

    /* chosen */
    node_hdr(6, 1);
    {
        const struct afdt_prop p[] = {
            {"name", STR("chosen")},
            {"firmware-version", STR("m1n1 lynx")},
            {"debug-enabled", &enabled, sizeof(enabled)},
            {"random-seed", seed, sizeof(seed)},
            {"dram-base", &dram_base, sizeof(dram_base)},
            {"dram-size", &dram_size, sizeof(dram_size)},
        };
        props(p, ARRAY_SIZE(p));
    }
    {
        /* arm_vm_prot_init asserts chosen/memory-map exists; empty is fine. */
        const struct afdt_prop p[] = {
            {"name", STR("memory-map")},
        };
        node_hdr(1, 0);
        props(p, ARRAY_SIZE(p));
    }

    /* defaults */
    node_hdr(2, 0);
    {
        const struct afdt_prop p[] = {
            {"name", STR("defaults")},
            {"vmm-present", &enabled, sizeof(enabled)},
        };
        props(p, ARRAY_SIZE(p));
    }

    /* cpus -> cpu0 */
    node_hdr(1, 1);
    {
        const struct afdt_prop p[] = {
            {"name", STR("cpus")},
        };
        props(p, ARRAY_SIZE(p));
    }
    node_hdr(4, 0);
    {
        const struct afdt_prop p[] = {
            {"name", STR("cpu0")},
            {"state", STR("running")},
            {"reg", &zero, sizeof(zero)},
            {"timebase-frequency", &timebase, sizeof(timebase)},
        };
        props(p, ARRAY_SIZE(p));
    }

    /*
     * arm-io -> gic.  No UART child: GS201's UART is
     * samsung,exynos-uart, which XNU has no driver for (it only knows
     * uart-1,samsung / arm,pl011 / dockchannel).  With no serial-device,
     * serial_init() cleanly reports no serial and the video console
     * stays the observation channel.
     */
    node_hdr(3, 1);
    {
        const struct afdt_prop p[] = {
            {"name", STR("arm-io")},
            {"device_type", STR("gs201-io")},
            {"ranges", arm_io_ranges, sizeof(arm_io_ranges)},
        };
        props(p, ARRAY_SIZE(p));
    }
    node_hdr(2, 0);
    {
        const struct afdt_prop p[] = {
            {"name", STR("gic")},
            {"reg", gic_reg, sizeof(gic_reg)},
        };
        props(p, ARRAY_SIZE(p));
    }

    *out_len = afdt_size;
    return afdt_data;
}

/*
 * Walk an AFDT node and return its encoded size, or 0 if the counts are
 * implausible.  Mirrors the U-Boot loader's validator.
 */
u32 gs201_afdt_length(const void *afdt)
{
    const u8 *p = afdt;
    u32 nprops = read32((u64)p);
    u32 nchildren = read32((u64)p + 4);
    u32 offset = 8;

    if (nprops > 4096 || nchildren > 4096)
        return 0;

    for (u32 i = 0; i < nprops; i++) {
        u32 len = read32((u64)p + offset + 32);
        offset += 32 + 4 + ((len + 3) & ~3u);
    }

    for (u32 i = 0; i < nchildren; i++) {
        u32 child = gs201_afdt_length(p + offset);

        if (!child)
            return 0;
        offset += child;
    }

    return offset;
}
