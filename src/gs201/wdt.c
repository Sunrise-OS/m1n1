/* SPDX-License-Identifier: MIT */

/*
 * Watchdogs and reset for GS201/lynx.
 *
 * ABL leaves both CPU-cluster watchdogs armed; the entry code clears WTCON
 * before touching DRAM, and this is here so the generic code (and reboots)
 * has the same calls it does on Apple.  Reset/poweroff go through PSCI, which
 * on GS201 is an SMC to the secure monitor.
 */

#include "gs201.h"

#include "types.h"
#include "utils.h"

#define PSCI_SYSTEM_OFF   0x84000008
#define PSCI_SYSTEM_RESET 0x84000009

void wdt_disable(void)
{
    write32(GS201_WDT_CLUSTER0, 0);
    write32(GS201_WDT_CLUSTER1, 0);
}

static void psci_call(u64 function)
{
    register u64 x0 asm("x0") = function;

    asm volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
}

void wdt_reboot(void)
{
    psci_call(PSCI_SYSTEM_RESET);

    for (;;)
        ;
}
