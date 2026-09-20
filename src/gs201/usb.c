/* SPDX-License-Identifier: MIT */

/*
 * USB device bring-up for GS201/lynx.
 *
 * ABL reaches us over USB (that is how `fastboot boot` delivered this image),
 * so the DWC3 core, its PHY and the HSI0 power domain are already up when we
 * start; we only take the core over and run m1n1's own CDC-ACM gadget on it so
 * the proxy is reachable over USB-C.  There is no DART/TZMP in front of the
 * controller from our point of view, so the generic DWC3 driver runs with a
 * NULL IOMMU (identity DMA).
 */

#include "gs201.h"

#include "exception.h"
#include "iodev.h"
#include "string.h"
#include "types.h"
#include "usb.h"
#include "usb_dwc3.h"
#include "utils.h"

/* DWC3 register window helpers (see usb_dwc3_regs.h). */
#define DWC3_GSNPSID      0xc120
#define DWC3_GSNPSID_MASK 0xffff0000

static dwc3_dev_t *usb_dev;

/*
 * Probe GSNPSID under the exception guard: the controller window may simply
 * not be mapped (everywhere but lynx), and an unguarded read would take an
 * external abort with the MMU off and reboot the board.
 */
static bool dwc3_present(void)
{
    enum exc_guard_t save = exc_guard;
    volatile u32 id;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    id = read32(gs201.usb_base + DWC3_GSNPSID);
    exc_guard = save;

    return (id & DWC3_GSNPSID_MASK) == 0x33310000;
}

static ssize_t usb_0_can_read(void *dev)
{
    return usb_dwc3_can_read(dev, CDC_ACM_PIPE_0);
}

static bool usb_0_can_write(void *dev)
{
    return usb_dwc3_can_write(dev, CDC_ACM_PIPE_0);
}

static ssize_t usb_0_read(void *dev, void *buf, size_t count)
{
    return usb_dwc3_read(dev, CDC_ACM_PIPE_0, buf, count);
}

static ssize_t usb_0_write(void *dev, const void *buf, size_t count)
{
    return usb_dwc3_write(dev, CDC_ACM_PIPE_0, buf, count);
}

static ssize_t usb_0_queue(void *dev, const void *buf, size_t count)
{
    return usb_dwc3_queue(dev, CDC_ACM_PIPE_0, buf, count);
}

static void usb_0_flush(void *dev)
{
    usb_dwc3_flush(dev, CDC_ACM_PIPE_0);
}

static void usb_0_handle_events(void *dev)
{
    usb_dwc3_handle_events(dev);
}

static ssize_t usb_sec_can_read(void *dev)
{
    return usb_dwc3_can_read(dev, CDC_ACM_PIPE_1);
}

static bool usb_sec_can_write(void *dev)
{
    return usb_dwc3_can_write(dev, CDC_ACM_PIPE_1);
}

static ssize_t usb_sec_read(void *dev, void *buf, size_t count)
{
    return usb_dwc3_read(dev, CDC_ACM_PIPE_1, buf, count);
}

static ssize_t usb_sec_write(void *dev, const void *buf, size_t count)
{
    return usb_dwc3_write(dev, CDC_ACM_PIPE_1, buf, count);
}

static ssize_t usb_sec_queue(void *dev, const void *buf, size_t count)
{
    return usb_dwc3_queue(dev, CDC_ACM_PIPE_1, buf, count);
}

static void usb_sec_flush(void *dev)
{
    usb_dwc3_flush(dev, CDC_ACM_PIPE_1);
}

static void usb_sec_handle_events(void *dev)
{
    usb_dwc3_handle_events(dev);
}

static struct iodev_ops iodev_usb_ops = {
    .can_read = usb_0_can_read,
    .can_write = usb_0_can_write,
    .read = usb_0_read,
    .write = usb_0_write,
    .queue = usb_0_queue,
    .flush = usb_0_flush,
    .handle_events = usb_0_handle_events,
};

static struct iodev_ops iodev_usb_sec_ops = {
    .can_read = usb_sec_can_read,
    .can_write = usb_sec_can_write,
    .read = usb_sec_read,
    .write = usb_sec_write,
    .queue = usb_sec_queue,
    .flush = usb_sec_flush,
    .handle_events = usb_sec_handle_events,
};

struct iodev iodev_usb_vuart = {
    .ops = &iodev_usb_sec_ops,
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

static struct iodev usb_iodev;

void usb_init(void)
{
    if (usb_dev)
        return;

    if (!gs201.usb_base) {
        printf("USB: no DWC3 controller in the firmware FDT\n");
        return;
    }

    if (!dwc3_present()) {
        printf("USB: no DWC3 core at 0x%lx\n", gs201.usb_base);
        return;
    }

    printf("USB: DWC3 at 0x%lx, GSNPSID=0x%lx\n", gs201.usb_base,
           (u64)read32(gs201.usb_base + DWC3_GSNPSID));

    usb_dev = usb_dwc3_init(gs201.usb_base, NULL);
    if (!usb_dev) {
        printf("USB: DWC3 init failed\n");
        return;
    }

    usb_iodev.ops = &iodev_usb_ops;
    usb_iodev.opaque = usb_dev;
    usb_iodev.usage = USAGE_CONSOLE | USAGE_UARTPROXY;
    spin_init(&usb_iodev.lock);
    iodev_register_device(IODEV_USB0, &usb_iodev);

    printf("USB: initialized\n");
}

void usb_iodev_init(void)
{
    usb_init();
}

void usb_iodev_shutdown(void)
{
    if (!usb_dev)
        return;

    iodev_unregister_device(IODEV_USB0);
    usb_dwc3_shutdown(usb_dev);
    usb_dev = NULL;
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}

void usb_hpm_restore_irqs(bool force)
{
    UNUSED(force);
}

bool gs201_usb_init(void)
{
    usb_init();
    return usb_dev != NULL;
}
