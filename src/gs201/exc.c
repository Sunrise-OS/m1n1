/* SPDX-License-Identifier: MIT */

/*
 * Exception handling for GS201/lynx.
 *
 * m1n1 keeps the monitor at EL2.  Two things share these vectors:
 *
 *  - the debug proxy's guarded memory access (exc_guard): a fault is caught,
 *    the faulting instruction skipped, and the proxy reports the failure;
 *
 *  - VMAPPLE XNU, which runs as an EL1 guest and issues HVCs that a real
 *    Apple hypervisor would service.  GS201 has none, so the HVC handler
 *    answers the CPU-service/PAC hypercalls just well enough for early boot
 *    to proceed (see gs201_hvc).
 *
 * The register save/restore trampolines are the shared ones in
 * exception_asm.S; only the C handlers are platform-specific.
 */

#include "exception.h"

#include "gs201.h"
#include "types.h"
#include "utils.h"

#define HVC_CPU_SERVICE_BASE 0xc1000000
#define HVC_CPU_SERVICE_LAST 0x6
#define HVC_PAC_NOP          0xc10000f0

#define ESR_EC_SERROR 0b101111

extern char _vectors_start[];

/* Referenced by the shared el0_call/el1_call trampolines in exception_asm.S. */
#define EL0_STACK_SIZE 0x4000
u8 el0_stack[EL0_STACK_SIZE] ALIGNED(64);
void *el0_stack_base = (void *)(u64)(&el0_stack[EL0_STACK_SIZE]);

volatile enum exc_guard_t exc_guard = GUARD_OFF;
volatile int exc_count = 0;

/* Pointer authentication emulation; see gs201_pac_trap. */
u32 gs201_pac_count;
u32 gs201_pac_key_count;

static u64 el_spsr(void)
{
    if (in_el3())
        return mrs(SPSR_EL3);
    if (in_el2())
        return mrs(SPSR_EL2);
    return mrs(SPSR_EL1);
}

static u64 el_esr(void)
{
    if (in_el3())
        return mrs(ESR_EL3);
    if (in_el2())
        return mrs(ESR_EL2);
    return mrs(ESR_EL1);
}

static u64 el_elr(void)
{
    if (in_el3())
        return mrs(ELR_EL3);
    if (in_el2())
        return mrs(ELR_EL2);
    return mrs(ELR_EL1);
}

static void el_set_elr(u64 elr)
{
    if (in_el3())
        msr(ELR_EL3, elr);
    else if (in_el2())
        msr(ELR_EL2, elr);
    else
        msr(ELR_EL1, elr);
}

static void el_set_spsr(u64 spsr)
{
    if (in_el3())
        msr(SPSR_EL3, spsr);
    else if (in_el2())
        msr(SPSR_EL2, spsr);
    else
        msr(SPSR_EL1, spsr);
}

static u64 el_far(void)
{
    if (in_el3())
        return mrs(FAR_EL3);
    if (in_el2())
        return mrs(FAR_EL2);
    return mrs(FAR_EL1);
}

void exception_initialize(void)
{
    if (in_el3())
        msr(VBAR_EL3, _vectors_start);
    else if (in_el2())
        msr(VBAR_EL2, _vectors_start);
    else
        msr(VBAR_EL1, _vectors_start);

    /* Mask everything: m1n1 polls the devices it cares about. */
    msr(CNTP_CTL_EL0, 7L);
    msr(CNTV_CTL_EL0, 7L);
    msr(DAIF, 0xf << 6);

    if (in_el2()) {
        /*
         * Plain EL2 (drop any VHE ABL left on), HVC enabled from EL1, no
         * trapping of PAC to EL2 so the guest executes it natively.
         */
        msr(HCR_EL2, BIT(31) | BIT(5)); /* RW | AMO */

        /* Let EL1 use the physical counter/timer; XNU drives its own. */
        msr(CNTHCTL_EL2, mrs(CNTHCTL_EL2) | 3);
        msr(CNTVOFF_EL2, 0);

        /* No FP/SIMD or debug-register traps for the guest. */
        msr(CPTR_EL2, 0x33ff);
        msr(HSTR_EL2, 0);
        msr(MDCR_EL2, 0);
    }

    sysop("isb");
}

void exception_shutdown(void)
{
    msr(DAIF, 0xf << 6);
}

/* Names for the ESR exception classes we can actually run into here. */
static const char *ec_name(u32 ec)
{
    switch (ec) {
        case 0x00: return "unknown";
        case 0x01: return "WFI/WFE";
        case 0x07: return "FP/SIMD trap";
        case 0x09: return "PAC instruction trap";
        case 0x0d: return "BTI";
        case 0x0e: return "illegal execution state";
        case 0x15: return "SVC";
        case 0x16: return "HVC";
        case 0x17: return "SMC";
        case 0x18: return "MSR/MRS/system trap";
        case 0x19: return "SVE trap";
        case 0x20: return "instruction abort, lower EL";
        case 0x21: return "instruction abort";
        case 0x22: return "PC alignment";
        case 0x24: return "data abort, lower EL";
        case 0x25: return "data abort";
        case 0x26: return "SP alignment";
        case 0x2c: return "FP trap";
        case 0x3c: return "BRK";
        default: return "reserved/unallocated";
    }
}

static const char *spsr_el_name(u32 m)
{
    switch (m) {
        case SPSR_M_EL0: return "EL0t";
        case SPSR_M_EL1T: return "EL1t";
        case SPSR_M_EL1H: return "EL1h";
        case 0b1000: return "EL2t (or EL1t+NV)";
        case 0b1001: return "EL2h (or EL1h+NV)";
        case 0b1100: return "EL3t";
        case 0b1101: return "EL3h";
        default: return "?";
    }
}

/*
 * The guest runs with its own MMU on, so the addresses in its registers mean
 * nothing outside its translation regime.  Walk its TTBR1 tables (4 KiB
 * granule) to see where it actually maps a VA -- that is what tells us
 * whether a fault is XNU's or the handoff's.
 */
static bool guest_va_to_pa(u64 va, u64 *pa_out)
{
    u64 tcr = mrs(TCR_EL1);
    u64 table = mrs(TTBR1_EL1) & GENMASK(47, 12);
    int va_bits = 64 - (int)((tcr >> 16) & 0x3f);
    int start = va_bits > 39 ? 0 : (va_bits > 30 ? 1 : 2);

    if (((tcr >> 30) & 3) != 2) /* only the 4 KiB granule */
        return false;

    for (int level = start; level <= 3; level++) {
        int shift = 12 + 9 * (3 - level);
        u64 desc = read64(table + ((va >> shift) & 0x1ff) * 8);
        u64 mask = GENMASK(47, shift);

        if (!(desc & 1))
            return false;

        if (level == 3) {
            *pa_out = (desc & GENMASK(47, 12)) | (va & 0xfff);
            return true;
        }

        if ((desc & 3) == 1) { /* block descriptor */
            *pa_out = (desc & mask) | (va & ~mask);
            return true;
        }

        table = desc & GENMASK(47, 12);
    }

    return false;
}

/*
 * A guest fault arrives here as a KVA that means nothing outside the image.
 * Translate it back to the offset into the XNU image (and the instruction
 * that was executing), which is what makes the dump actionable.
 */
static void print_guest_fault(u64 elr)
{
    u64 off, pa;

    if (!gs201.guest_image_size || elr < gs201.guest_virt_base)
        return;

    off = elr - gs201.guest_virt_base;
    if (off >= gs201.guest_image_size)
        return;

    printf("PC in XNU image: +0x%lx (insn 0x%08lx)\n", off,
           (u64)read32(gs201.guest_phys_base + off));

    if (guest_va_to_pa(elr, &pa)) {
        printf("Guest TTBR1 maps it to PA 0x%lx: insn 0x%08lx (image +0x%lx)\n", pa,
               (u64)read32(pa), pa - gs201.guest_phys_base);
    } else {
        printf("Guest TTBR1 has no translation for it\n");
    }
}

void print_regs(u64 *regs, int el12)
{
    UNUSED(el12);

    u64 spsr = el_spsr();
    u64 elr = el_elr();
    u64 esr = el_esr();

    printf("Exception taken from %s\n", spsr_el_name(spsr & SPSR_M_EL));
    printf("Running in EL%lu\n", mrs(CurrentEL) >> 2);
    printf("MPIDR: 0x%lx\n", mrs(MPIDR_EL1));
    printf("Registers: (@%p)\n", regs);
    printf("  x0-x3: %016lx %016lx %016lx %016lx\n", regs[0], regs[1], regs[2], regs[3]);
    printf("  x4-x7: %016lx %016lx %016lx %016lx\n", regs[4], regs[5], regs[6], regs[7]);
    printf(" x8-x11: %016lx %016lx %016lx %016lx\n", regs[8], regs[9], regs[10], regs[11]);
    printf("x12-x15: %016lx %016lx %016lx %016lx\n", regs[12], regs[13], regs[14], regs[15]);
    printf("x16-x19: %016lx %016lx %016lx %016lx\n", regs[16], regs[17], regs[18], regs[19]);
    printf("x20-x23: %016lx %016lx %016lx %016lx\n", regs[20], regs[21], regs[22], regs[23]);
    printf("x24-x27: %016lx %016lx %016lx %016lx\n", regs[24], regs[25], regs[26], regs[27]);
    printf("x28-x30: %016lx %016lx %016lx\n", regs[28], regs[29], regs[30]);
    printf("PC:       0x%lx (rel: 0x%lx)\n", elr, elr - (u64)_base);
    printf("SPSR:     0x%lx\n", spsr);
    printf("FAR:      0x%lx\n", el_far());
    printf("HCR_EL2:  0x%lx\n", in_el2() ? mrs(HCR_EL2) : 0UL);
    printf("PAC:      %lu instructions, %lu key-register accesses emulated\n",
           (u64)gs201_pac_count, (u64)gs201_pac_key_count);
    printf("ESR:      0x%lx (EC 0x%lx %s, ISS 0x%lx)\n", esr, (esr & ESR_EC) >> ESR_EC_SHIFT,
           ec_name((esr & ESR_EC) >> ESR_EC_SHIFT), esr & GENMASK(24, 0));
    print_guest_fault(elr);
}

/* ---- Pointer authentication -------------------------------------------- */

/*
 * An arm64e kernel signs and authenticates pointers with keys that an Apple
 * hypervisor hands over through PAC_GET_DEFAULT_KEYS.  GS201 has no such keys,
 * so the guest is entered with HCR_EL2.API/APK clear, which turns every PAC
 * instruction and key-register access into a trap to EL2 (EC 0x09 and 0x18)
 * and the shim answers them itself:
 *
 *  - authentication always succeeds, and *strips* the pointer back to the
 *    canonical address for the regime in use.  That is what a successful AUT
 *    does, and it also makes pointers the kernel's own build signed with
 *    Apple's keys usable.
 *  - signing is a no-op: anything this shim signs is only ever handed back to
 *    its own authentication, which strips it.
 *  - PAC key registers are discarded, and read back as zero.
 *
 * The trap's ISS carries no instruction information, so the instruction is
 * read from the guest's PC.
 */

/* Data-processing forms: operands are bits [4:0] (Rd) and [9:5] (Rn). */
#define PAC_OP_MASK 0xfffffc00u
#define PAC_PACIA   0xdac10000u
#define PAC_PACIB   0xdac10400u
#define PAC_PACDA   0xdac10800u
#define PAC_PACDB   0xdac10c00u
#define PAC_AUTIA   0xdac11000u
#define PAC_AUTIB   0xdac11400u
#define PAC_AUTDA   0xdac11800u
#define PAC_AUTDB   0xdac11c00u
#define PAC_PACIZA  0xdac12000u
#define PAC_PACIZB  0xdac12400u
#define PAC_PACDZA  0xdac12800u
#define PAC_PACDZB  0xdac12c00u
#define PAC_AUTIZA  0xdac13000u
#define PAC_AUTIZB  0xdac13400u
#define PAC_AUTDZA  0xdac13800u
#define PAC_AUTDZB  0xdac13c00u
#define PAC_XPACI   0xdac14000u
#define PAC_XPACD   0xdac14400u
#define PAC_BRAA    0xd71f0800u
#define PAC_BRAB    0xd71f0c00u
#define PAC_BLRAA   0xd73f0800u
#define PAC_BLRAB   0xd73f0c00u
#define PAC_BRAAZ   0xd61f081fu
#define PAC_BRABZ   0xd61f0c1fu
#define PAC_BLRAAZ  0xd63f081fu
#define PAC_BLRABZ  0xd63f0c1fu

/* Operand-less forms.  The SP/Z encodings act on x30, the 1716 ones on x16. */
#define PAC_PACIASP   0xd503233fu
#define PAC_PACIBSP   0xd503237fu
#define PAC_PACIAZ    0xd503231fu
#define PAC_PACIBZ    0xd503235fu
#define PAC_PACIA1716 0xd503211fu
#define PAC_PACIB1716 0xd503215fu
#define PAC_AUTIASP   0xd50323bfu
#define PAC_AUTIBSP   0xd50323ffu
#define PAC_AUTIAZ    0xd503239fu
#define PAC_AUTIBZ    0xd50323dfu
#define PAC_AUTIA1716 0xd503219fu
#define PAC_AUTIB1716 0xd50321dfu
#define PAC_XPACLRI   0xd50320ffu
#define PAC_RETAA     0xd65f0bffu
#define PAC_RETAB     0xd65f0fffu

/* NZCV after a successful authentication: N=0, Z=1, C=1, V=0. */
#define SPSR_NZCV_AUTH_OK (0x6UL << 28)

/*
 * What a successful authentication restores is the sign extension of the
 * pointer above the address size of the regime it belongs to.  Bit 55
 * survives both the address and any tag byte, so it picks the regime.
 */
static u64 pac_strip(u64 ptr)
{
    u32 tsz = 17;

    if (mrs(SCTLR_EL1) & 1) {
        u64 tcr = mrs(TCR_EL1);
        tsz = (ptr & BIT(55)) ? (u32)((tcr >> 16) & 0x3f) : (u32)(tcr & 0x3f);
        if (tsz == 0 || tsz > 40)
            tsz = 17;
    }

    int bits = 64 - (int)tsz;
    u64 top = BIT(bits - 1);

    return (ptr & top) ? (ptr | ~(top - 1)) : (ptr & (top - 1));
}

static u32 guest_fetch(u64 va)
{
    /*
     * With the guest's MMU on, its image is mapped at a constant offset from
     * where this loader put it, so code being executed can be read through
     * the image; with the MMU off the address is already physical.
     */
    if ((mrs(SCTLR_EL1) & 1) && gs201.guest_image_size && va >= gs201.guest_virt_base &&
        va < gs201.guest_virt_base + gs201.guest_image_size)
        return read32(gs201.guest_phys_base + (va - gs201.guest_virt_base));

    return read32(va);
}

static void gs201_pac_trap(u64 *regs, u64 elr, u64 spsr)
{
    u32 insn = guest_fetch(elr);
    u32 op = insn & PAC_OP_MASK;
    u32 rd = insn & 0x1f;
    u32 rn = (insn >> 5) & 0x1f;
    u64 next = elr + 4;

    if (!gs201_pac_count++)
        printf("PAC: emulating pointer authentication (first trap at 0x%lx)\n", elr);

    switch (op) {
        /* Signing changes nothing we later check. */
        case PAC_PACIA:
        case PAC_PACIB:
        case PAC_PACDA:
        case PAC_PACDB:
        case PAC_PACIZA:
        case PAC_PACIZB:
        case PAC_PACDZA:
        case PAC_PACDZB:
            break;

        /* Authentication: strip, and report success in the flags. */
        case PAC_AUTIA:
        case PAC_AUTIB:
        case PAC_AUTDA:
        case PAC_AUTDB:
        case PAC_AUTIZA:
        case PAC_AUTIZB:
        case PAC_AUTDZA:
        case PAC_AUTDZB:
            regs[rd] = pac_strip(regs[rd]);
            el_set_spsr((spsr & ~GENMASK(31, 28)) | SPSR_NZCV_AUTH_OK);
            break;

        /* Strips are hints: no flags. */
        case PAC_XPACI:
        case PAC_XPACD:
            regs[rd] = pac_strip(regs[rd]);
            break;

        case PAC_BRAA:
        case PAC_BRAB:
            next = pac_strip(regs[rn]);
            break;

        case PAC_BLRAA:
        case PAC_BLRAB:
            regs[30] = elr + 4;
            next = pac_strip(regs[rn]);
            break;
        default: {
            u32 op_z = insn & ~0x3e0u;

            if (op_z == PAC_BRAAZ || op_z == PAC_BRABZ) {
                next = pac_strip(regs[rn]);
                break;
            }
            if (op_z == PAC_BLRAAZ || op_z == PAC_BLRABZ) {
                regs[30] = elr + 4;
                next = pac_strip(regs[rn]);
                break;
            }

            switch (insn) {
                case PAC_PACIASP:
                case PAC_PACIBSP:
                case PAC_PACIAZ:
                case PAC_PACIBZ:
                case PAC_PACIA1716:
                case PAC_PACIB1716:
                    break;

                case PAC_AUTIASP:
                case PAC_AUTIBSP:
                case PAC_AUTIAZ:
                case PAC_AUTIBZ:
                    regs[30] = pac_strip(regs[30]);
                    el_set_spsr((spsr & ~GENMASK(31, 28)) | SPSR_NZCV_AUTH_OK);
                    break;

                case PAC_AUTIA1716:
                case PAC_AUTIB1716:
                    regs[16] = pac_strip(regs[16]);
                    el_set_spsr((spsr & ~GENMASK(31, 28)) | SPSR_NZCV_AUTH_OK);
                    break;

                case PAC_XPACLRI:
                    regs[30] = pac_strip(regs[30]);
                    break;

                case PAC_RETAA:
                case PAC_RETAB:
                    next = pac_strip(regs[30]);
                    break;

                default:
                    printf("PAC: unhandled instruction 0x%08x at 0x%lx\n", insn, elr);
                    break;
            }
            break;
        }
    }

    el_set_elr(next);
}

/*
 * MSR/MRS trap (EC 0x18).  The only accesses this shim causes are the guest's
 * writes to the PAC key registers, which are S3_0_C2_C1_0..4.
 */
static bool gs201_sysreg_trap(u64 *regs, u64 esr, u64 elr)
{
    u32 iss = esr & GENMASK(24, 0);
    u32 op0 = (iss >> 20) & 3;
    u32 op2 = (iss >> 17) & 7;
    u32 op1 = (iss >> 14) & 7;
    u32 crn = (iss >> 10) & 0xf;
    u32 rt = (iss >> 5) & 0x1f;
    u32 crm = (iss >> 1) & 0xf;

    if (op0 != 3 || op1 != 0 || crn != 2 || crm != 1 || op2 > 4)
        return false;

    if (iss & 1)
        regs[rt] = 0; /* reading a key register: we hold none */

    gs201_pac_key_count++;
    el_set_elr(elr + 4);

    return true;
}

/*
 * Minimal VMAPPLE EL2 stand-in.  XNU's PAC key setters spin on a non-zero
 * result and the OEM calls are guarded by hvg_is_hcall_available(), so
 * returning success for the CPU-service range and failure for everything
 * else (PAC_NOP included) lets early boot proceed without emulating a real
 * hypervisor.
 */
static void gs201_hvc(u64 *regs)
{
    u64 id = regs[0];

    if (id >= HVC_CPU_SERVICE_BASE && id <= HVC_CPU_SERVICE_BASE + HVC_CPU_SERVICE_LAST) {
        regs[0] = 0;
        if (id == HVC_CPU_SERVICE_BASE + 1) {
            /* PAC_GET_DEFAULT_KEYS consumes x2/x3 as key material. */
            regs[2] = 0;
            regs[3] = 0;
        }
        return;
    }

    if (id != HVC_PAC_NOP)
        printf("HVC 0x%lx unhandled\n", id);

    regs[0] = (u64)-1;
}

void exc_sync(u64 *regs)
{
    u64 spsr = el_spsr();
    u64 esr = el_esr();
    u64 elr = el_elr();
    u32 ec = (esr & ESR_EC) >> ESR_EC_SHIFT;
    u32 elsp = spsr & SPSR_M_EL;

    if (ec == ESR_EC_HVC && in_el2() && (elsp == SPSR_M_EL1H || elsp == SPSR_M_EL1T)) {
        gs201_hvc(regs);
        return;
    }

    if (in_el2() && (elsp == SPSR_M_EL1H || elsp == SPSR_M_EL1T)) {
        /* PAC instructions, and the PAC key registers, are the shim's. */
        if (ec == ESR_EC_PAUTH_TRAP) {
            gs201_pac_trap(regs, elr, spsr);
            return;
        }
        if (ec == ESR_EC_MSR && gs201_sysreg_trap(regs, esr, elr))
            return;
    }

    if (!(exc_guard & GUARD_SILENT))
        printf("Exception: SYNC (EC 0x%x) ELR=0x%lx\n", ec, elr);

    switch (exc_guard & GUARD_TYPE_MASK) {
        case GUARD_SKIP:
            elr += 4;
            break;
        case GUARD_MARK: {
            u32 insn = read32(elr);
            regs[insn & 0x1f] = 0xacce5515abad1dea;
            elr += 4;
            break;
        }
        case GUARD_RETURN:
            regs[0] = 0xacce5515abad1dea;
            elr = regs[30];
            exc_guard = GUARD_OFF;
            break;
        case GUARD_OFF:
        default:
            print_regs(regs, 0);
            printf("Unhandled exception, rebooting...\n");
            flush_and_reboot();
    }

    exc_count++;
    el_set_elr(elr);

    if (!(exc_guard & GUARD_SILENT))
        printf("Recovering from exception (ELR=0x%lx)\n", elr);
}

void exc_irq(u64 *regs)
{
    UNUSED(regs);
    printf("Exception: IRQ at EL%lu (unexpected)\n", mrs(CurrentEL) >> 2);
}

void exc_fiq(u64 *regs)
{
    UNUSED(regs);
    printf("Exception: FIQ at EL%lu (unexpected)\n", mrs(CurrentEL) >> 2);

    u64 ctl = mrs(CNTP_CTL_EL0);
    if (ctl & 1)
        msr(CNTP_CTL_EL0, 7L);
    ctl = mrs(CNTV_CTL_EL0);
    if (ctl & 1)
        msr(CNTV_CTL_EL0, 7L);
}

void exc_serr(u64 *regs)
{
    if (!(exc_guard & GUARD_SILENT)) {
        printf("Exception: SError\n");
        print_regs(regs, 0);
    }

    if ((exc_guard & GUARD_TYPE_MASK) == GUARD_OFF) {
        printf("Unhandled SError, rebooting...\n");
        flush_and_reboot();
    }

    exc_count++;
}

/* Called from the vectors for a synchronous exception from EL2 itself. */
void gs201_exc_init(void)
{
    exception_initialize();
}
