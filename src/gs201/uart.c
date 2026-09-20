/* SPDX-License-Identifier: MIT */

/*
 * UART driver for GS201/lynx.
 *
 * The Pixel 7a exposes no UART: the real consoles are the panel and USB-C.
 * This exists only so m1n1 can also run under QEMU (where the devicetree
 * describes an ARM PL011) and as a last-resort debug sink if the firmware
 * left the on-SoC USI UART running.  Because it is never the console anyone
 * depends on, every poll is bounded: if the UART does not respond it is
 * switched off instead of hanging the boot.
 *
 * No interrupts; everything is polled like the rest of m1n1.
 */

#include <stdarg.h>

#include "gs201.h"

#include "iodev.h"
#include "types.h"
#include "uart.h"
#include "utils.h"
#include "vsprintf.h"

/* Samsung USI UART */
#define UTRSTAT 0x010
#define UTXH    0x020
#define URXH    0x024

#define UTRSTAT_TXBE BIT(1)
#define UTRSTAT_TXE  BIT(2)
#define UTRSTAT_RXD  BIT(0)

/* ARM PL011 */
#define PL011_DR    0x000
#define PL011_FR    0x018
#define PL011_IBRD  0x024
#define PL011_FBRD  0x028
#define PL011_LCR_H 0x02c
#define PL011_CR    0x030
#define PL011_ICR   0x044

#define PL011_FR_TXFE BIT(7)
#define PL011_FR_TXFF BIT(5)
#define PL011_FR_RXFE BIT(4)
#define PL011_CR_UARTEN BIT(0)
#define PL011_CR_TXE    BIT(8)
#define PL011_CR_RXE    BIT(9)

int gs201_uart_init(void)
{
    u64 base = gs201.uart_base;

    if (!base)
        return -1;

    if (gs201.uart_is_pl011) {
        write32(base + PL011_CR, 0);
        /* 115200 baud: divisor = clk / (16 * baud); GS201's PL011 input is 24 MHz. */
        u32 clk = gs201.uart_clock ? gs201.uart_clock : 24000000;
        u32 divisor = (clk * 4 + 115200 / 2) / 115200;
        write32(base + PL011_IBRD, divisor >> 6);
        write32(base + PL011_FBRD, divisor & 0x3f);
        write32(base + PL011_LCR_H, 0x70); /* 8n1, FIFOs */
        write32(base + PL011_ICR, 0x7ff);
        write32(base + PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
    }

    return 0;
}

/* Bounded poll: a dead UART must not wedge the boot. */
#define UART_POLL_TIMEOUT 100000

static bool uart_tx_ready(void)
{
    u64 base = gs201.uart_base;

    if (!base)
        return false;

    if (gs201.uart_is_pl011)
        return !(read32(base + PL011_FR) & PL011_FR_TXFF);

    return read32(base + UTRSTAT) & UTRSTAT_TXBE;
}

void uart_putbyte(u8 c)
{
    u64 base = gs201.uart_base;

    if (!base)
        return;

    for (int i = 0; !uart_tx_ready(); i++) {
        if (i >= UART_POLL_TIMEOUT) {
            gs201.uart_base = 0;
            return;
        }
    }

    if (gs201.uart_is_pl011)
        write32(base + PL011_DR, c);
    else
        write32(base + UTXH, c);
}

u8 uart_getbyte(void)
{
    u64 base = gs201.uart_base;

    if (!base)
        return 0;

    if (gs201.uart_is_pl011) {
        while (read32(base + PL011_FR) & PL011_FR_RXFE)
            ;
        return read32(base + PL011_DR);
    }

    while (!(read32(base + UTRSTAT) & UTRSTAT_RXD))
        ;
    return read32(base + URXH);
}

void uart_putchar(u8 c)
{
    if (c == '\n')
        uart_putbyte('\r');
    uart_putbyte(c);
}

u8 uart_getchar(void)
{
    return uart_getbyte();
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putchar(*(s++));
    uart_putchar('\n');
}

void uart_write(const void *buf, size_t count)
{
    const u8 *p = buf;

    while (count--)
        uart_putbyte(*p++);
}

size_t uart_read(void *buf, size_t count)
{
    u8 *p = buf;
    size_t recvd = 0;

    while (count--) {
        *p++ = uart_getbyte();
        recvd++;
    }

    return recvd;
}

void uart_setbaud(int baudrate)
{
    UNUSED(baudrate);
    /* The console UART is shared with the bootloader; leave its clock alone. */
}

void uart_flush(void)
{
    u64 base = gs201.uart_base;

    if (!base)
        return;

    for (int i = 0; i < UART_POLL_TIMEOUT; i++) {
        bool done = gs201.uart_is_pl011 ? (read32(base + PL011_FR) & PL011_FR_TXFE)
                                        : (read32(base + UTRSTAT) & UTRSTAT_TXE);
        if (done)
            return;
    }

    gs201.uart_base = 0;
}

void uart_clear_irqs(void)
{
    if (gs201.uart_base && gs201.uart_is_pl011)
        write32(gs201.uart_base + PL011_ICR, 0x7ff);
}

int uart_init(void)
{
    return gs201_uart_init();
}

int uart_printf(const char *fmt, ...)
{
    va_list args;
    char buffer[512];
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    uart_write(buffer, min(i, (int)(sizeof(buffer) - 1)));

    return i;
}

static bool uart_iodev_can_write(void *opaque)
{
    UNUSED(opaque);
    return gs201.uart_base != 0;
}

static ssize_t uart_iodev_can_read(void *opaque)
{
    UNUSED(opaque);

    if (!gs201.uart_base)
        return 0;

    if (gs201.uart_is_pl011)
        return (read32(gs201.uart_base + PL011_FR) & PL011_FR_RXFE) ? 0 : 1;

    return (read32(gs201.uart_base + UTRSTAT) & UTRSTAT_RXD) ? 1 : 0;
}

static ssize_t uart_iodev_read(void *opaque, void *buf, size_t len)
{
    UNUSED(opaque);
    return uart_read(buf, len);
}

static ssize_t uart_iodev_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    uart_write(buf, len);
    return len;
}

static struct iodev_ops iodev_uart_ops = {
    .can_read = uart_iodev_can_read,
    .can_write = uart_iodev_can_write,
    .read = uart_iodev_read,
    .write = uart_iodev_write,
};

struct iodev iodev_uart = {
    .ops = &iodev_uart_ops,
    .usage = USAGE_CONSOLE | USAGE_UARTPROXY,
    .lock = SPINLOCK_INIT,
};
