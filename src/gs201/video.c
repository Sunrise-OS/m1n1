/* SPDX-License-Identifier: MIT */

/*
 * FB console bring-up on GS201/lynx.
 *
 * ABL leaves the whole display pipeline (DPP/DECON/MIPI-DSIM and the panel)
 * running with its splash scanned out of an RDMA plane.  There is no
 * simple-framebuffer node and no reason to reprogram DSIM, so like the U-Boot
 * port we simply adopt that plane: find the live RDMA channel, point it at the
 * fixed scrape buffer ABL uses and force a linear XRGB8888 scanout from it.
 * The panel keeps scanning no matter what we do afterwards.
 *
 * Everything here runs with the MMU and caches off, exactly like ABL left it,
 * which is also why the register pokes are plain read/write32 helpers.
 */

#include "gs201.h"

#include "types.h"
#include "utils.h"

#define GS201_DPU_DMA  0x1c0b0000UL
#define GS201_DECON0   0x1c240000UL
#define CHANNEL_STRIDE 0x1000
#define CHANNELS       6
#define DMA_SHD_OFFSET 0x0400

/* RDMA (DPP DMA) registers */
#define RDMA_ENABLE    0x0000
#define RDMA_IN_CTRL_0 0x0008
#define RDMA_SRC_SIZE  0x0010
#define RDMA_IMG_SIZE  0x0018
#define RDMA_BASEADDR  0x0040
#define RDMA_STRIDE_0  0x0050
#define RDMA_STRIDE_1  0x0054

/* DECON registers */
#define DECON_GLOBAL_CON     0x0020
#define DECON_TRIG_CON       0x0030
#define DECON_TRIG_CON_SEC   0x003c
#define DECON_SHD_REG_UP_REQ 0x0050

#define IDMA_SFR_UPDATE_FORCE    BIT(4)
#define IDMA_IMG_FORMAT_XRGB8888 (7u << 8)
#define IDMA_STRIDE_0_SEL        BIT(20)

#define DECON_EN        BIT(1)
#define DECON_EN_F      BIT(0)
#define SW_TRIG_EN      BIT(8)
#define SW_TRIG_DET_EN  BIT(1)
#define HW_TRIG_EN      BIT(0)
#define HW_TRIG_MASK_DECON BIT(4)

#define SHD_GLOBAL      BIT(31)
#define SHD_FOR_DECON   0x3f

static void wr_shadow(u64 base, u32 off, u32 val)
{
    write32(base + off, val);
    write32(base + off + DMA_SHD_OFFSET, val);
}

static u64 find_live_rdma(u32 *width, u32 *height)
{
    for (unsigned int ch = 0; ch < CHANNELS; ch++) {
        u64 candidate = GS201_DPU_DMA + ch * CHANNEL_STRIDE;
        u32 img = read32(candidate + RDMA_IMG_SIZE);
        u32 src = read32(candidate + RDMA_SRC_SIZE);
        u32 w = img & 0x3fff;
        u32 h = (img >> 16) & 0x3fff;

        if (!w || !h) {
            w = src & 0xffff;
            h = (src >> 16) & 0xffff;
        }

        if (w >= 16 && w <= 4096 && h >= 16 && h <= 4096 &&
            read32(candidate + RDMA_BASEADDR)) {
            *width = w;
            *height = h;
            return candidate;
        }
    }

    *width = GS201_FB_WIDTH;
    *height = GS201_FB_HEIGHT;
    return GS201_DPU_DMA;
}

int gs201_video_init(void)
{
    u32 width, height;

    /*
     * Only touch the DPU when the firmware DT actually describes it.  ABL's
     * DT does; a QEMU dev tree does not, and probing live DECON registers
     * there would take an external abort with the MMU off.
     */
    if (!gs201_fdt_compatible("samsung,exynos-decon") &&
        !gs201_fdt_compatible("google,gs201-dpu"))
        return -1;

    u64 dma = find_live_rdma(&width, &height);

    gs201.fb_base = GS201_FB_BASE;
    gs201.fb_stride = width * 4;
    gs201.fb_width = width;
    gs201.fb_height = height;
    gs201.fb_depth = 32;
    gs201.video = true;

    /* Force a linear XRGB8888 scanout out of the fixed ABL buffer. */
    u32 ctrl = read32(dma + RDMA_IN_CTRL_0);
    ctrl &= ~(BIT(0) | BIT(1) | BIT(2) | (0x3f << 8));
    ctrl |= (7u << 8); /* IDMA_IMG_FORMAT_XRGB8888: memory bytes X, R, G, B */
    wr_shadow(dma, RDMA_IN_CTRL_0, ctrl);

    wr_shadow(dma, RDMA_BASEADDR, GS201_FB_BASE);

    u32 stride0 = read32(dma + RDMA_STRIDE_0) | IDMA_STRIDE_0_SEL;
    wr_shadow(dma, RDMA_STRIDE_0, stride0);
    wr_shadow(dma, RDMA_STRIDE_1, width * 4);

    u32 size = (height << 16) | width;
    wr_shadow(dma, RDMA_SRC_SIZE, size);
    wr_shadow(dma, RDMA_IMG_SIZE, size);
    /* Clean dcache across the linear FB memory so DECON DMA reads clean memory */
    u8 *fb_mem = (u8 *)GS201_FB_BASE;
    size_t fb_bytes = width * height * 4;
    for (size_t i = 0; i < fb_bytes; i += 64)
        dc_cvac(fb_mem + i);
    sysop("dsb sy");
    write32(dma + RDMA_ENABLE, read32(dma + RDMA_ENABLE) | IDMA_SFR_UPDATE_FORCE);

    write32(GS201_DECON0 + DECON_SHD_REG_UP_REQ, SHD_GLOBAL | SHD_FOR_DECON);

    u32 g = read32(GS201_DECON0 + DECON_GLOBAL_CON);
    write32(GS201_DECON0 + DECON_GLOBAL_CON, g | DECON_EN | DECON_EN_F);

    u32 t = read32(GS201_DECON0 + DECON_TRIG_CON_SEC);
    write32(GS201_DECON0 + DECON_TRIG_CON_SEC, t & ~HW_TRIG_MASK_DECON);

    t = read32(GS201_DECON0 + DECON_TRIG_CON);
    write32(GS201_DECON0 + DECON_TRIG_CON,
            (t & ~HW_TRIG_MASK_DECON) | SW_TRIG_EN | SW_TRIG_DET_EN | HW_TRIG_EN);

    printf("gs201: DECON adopted, %ux%u XRGB8888 at 0x%lx\n", width, height,
           (u64)GS201_FB_BASE);

    return 0;
}
