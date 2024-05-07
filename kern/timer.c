#include <inc/assert.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/types.h>
#include <inc/uefi.h>
#include <inc/x86.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/trap.h>
#include <kern/traceopt.h>
#include <kern/pmap.h>
#include <kern/timer.h>
#include <kern/tsc.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}
void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

static uint8_t
calc_checksum(uint8_t *ptr, uint64_t size) {
    uint8_t result = 0;

    for (uint64_t i = 0; i < size; ++i) {
        result += ptr[i];
    }

    return result;
}

static void
validate_table_checksum(void *table, uint64_t size) {
    uint8_t checksum = calc_checksum((uint8_t*)table, size);
    assert(checksum == 0);
}

static void
print_sdt_header(ACPISDTHeader *h) {
    if (trace_acpi) {
        cprintf("---SDT---\n");
        cprintf("SDT: Signature:       %.4s\n", h->Signature);
        cprintf("SDT: Length:          %u\n", h->Length);
        cprintf("SDT: Revision:        %hhu\n", h->Revision);
        cprintf("SDT: Checksum:        %hhu\n", h->Checksum);
        cprintf("SDT: OEMID:           %.6s\n", h->OEMID);
        cprintf("SDT: OEMTableID:      %.8s\n", h->OEMTableID);
        cprintf("SDT: OEMRevision:     %u\n", h->OEMRevision);
        cprintf("SDT: CreatorID:       %u\n", h->CreatorID);
        cprintf("SDT: CreatorRevision: %u\n", h->CreatorRevision);
    }
}

static void
print_rsdp(RSDP *rsdp) {
    if (trace_acpi) {
        cprintf("RSDP: sizeof:           %lu\n", sizeof(RSDP));
        cprintf("RSDP: Signature:        %.8s\n", rsdp->Signature);
        cprintf("RSDP: Checksum:         %hhu\n", rsdp->Checksum);
        cprintf("RSDP: OEMID:            %.8s\n", rsdp->OEMID);
        cprintf("RSDP: Revision:         %hhu\n", rsdp->Revision);
        cprintf("RSDP: RsdtAddress:      0x%08x\n", rsdp->RsdtAddress);
        cprintf("RSDP: Length:           %u\n", rsdp->Length);
        cprintf("RSDP: XsdtAddress:      0x%08lx\n", rsdp->XsdtAddress);
        cprintf("RSDP: ExtendedChecksum: %hhu\n", rsdp->ExtendedChecksum);
    }
}

static void *
acpi_find_table(const char *sign) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is requrired?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */
    // LAB 5: Your code here:

    // TODO(e-kutovoi): Should this data be cached?

    RSDP *rsdp = mmio_map_region(uefi_lp->ACPIRoot, sizeof(RSDP));

    // NOTE: Not handling ACPI 1.0 yet. All code below assumes revision >= 2
    assert(rsdp->Revision >= 2);

    print_rsdp(rsdp);

    assert(rsdp->Length == sizeof(RSDP));

    validate_table_checksum(rsdp, 20);
    validate_table_checksum(rsdp, sizeof(RSDP));

    // NOTE: Not going to use
    // RSDT *rsdt = mmio_map_region(rsdp->RsdtAddress, sizeof(ACPISDTHeader));
    // print_sdt_header(&rsdt->h);

    XSDT *xsdt = mmio_map_region(rsdp->XsdtAddress, sizeof(ACPISDTHeader));
    print_sdt_header(&xsdt->h);
    validate_table_checksum(xsdt, xsdt->h.Length);
    // Remap at full length (even though mmio_remap_last_region == mmio_map_region)
    xsdt = mmio_remap_last_region(rsdp->XsdtAddress, xsdt, sizeof(ACPISDTHeader), xsdt->h.Length);

    uint64_t num_of_entries = (xsdt->h.Length - sizeof(ACPISDTHeader)) / sizeof(uint64_t);

    if_cprintf(trace_acpi, "XSDT: NumOfEntries: %lu\n", num_of_entries);

    for (uint64_t i = 0; i < num_of_entries; ++i) {
        uint64_t addr = xsdt->PointerToOtherSDT[i];
        ACPISDTHeader *table = mmio_map_region(addr, sizeof(ACPISDTHeader));

        if_cprintf(trace_acpi, "SDT[%lu]:\n", i);
        print_sdt_header(table);

        table = mmio_remap_last_region(addr, table, sizeof(ACPISDTHeader), table->Length);
        validate_table_checksum(table, ((ACPISDTHeader*)table)->Length);

        if (strncmp(table->Signature, sign, 4) == 0) {
            return table;
        }
    }

    return NULL;
}

MCFG *
get_mcfg(void) {
    static MCFG *kmcfg;
    if (!kmcfg) {
        struct AddressSpace *as = switch_address_space(&kspace);
        kmcfg = acpi_find_table("MCFG");
        switch_address_space(as);
    }

    return kmcfg;
}

#define MAX_SEGMENTS 16

uintptr_t
make_fs_args(char *ustack_top) {

    MCFG *mcfg = get_mcfg();
    if (!mcfg) {
        cprintf("MCFG table is absent!");
        return (uintptr_t)ustack_top;
    }

    char *argv[MAX_SEGMENTS + 3] = {0};

    /* Store argv strings on stack */

    ustack_top -= 3;
    argv[0] = ustack_top;
    nosan_memcpy(argv[0], "fs", 3);

    int nent = (mcfg->h.Length - sizeof(MCFG)) / sizeof(CSBAA);
    if (nent > MAX_SEGMENTS)
        nent = MAX_SEGMENTS;

    for (int i = 0; i < nent; i++) {
        CSBAA *ent = &mcfg->Data[i];

        char arg[64];
        snprintf(arg, sizeof(arg) - 1, "ecam=%llx:%04x:%02x:%02x",
                 (long long)ent->BaseAddress, ent->SegmentGroup, ent->StartBus, ent->EndBus);

        int len = strlen(arg) + 1;
        ustack_top -= len;
        nosan_memcpy(ustack_top, arg, len);
        argv[i + 1] = ustack_top;
    }

    char arg[64];
    snprintf(arg, sizeof(arg) - 1, "tscfreq=%llx", (long long)tsc_calibrate());
    int len = strlen(arg) + 1;
    ustack_top -= len;
    nosan_memcpy(ustack_top, arg, len);
    argv[nent + 1] = ustack_top;

    /* Realign stack */
    ustack_top = (char *)((uintptr_t)ustack_top & ~(2 * sizeof(void *) - 1));

    /* Copy argv vector */
    ustack_top -= (nent + 3) * sizeof(void *);
    nosan_memcpy(ustack_top, argv, (nent + 3) * sizeof(argv[0]));

    char **argv_arg = (char **)ustack_top;
    long argc_arg = nent + 2;

    /* Store argv and argc arguemnts on stack */
    ustack_top -= sizeof(void *);
    nosan_memcpy(ustack_top, &argv_arg, sizeof(argv_arg));
    ustack_top -= sizeof(void *);
    nosan_memcpy(ustack_top, &argc_arg, sizeof(argc_arg));

    /* and return new stack pointer */
    return (uintptr_t)ustack_top;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    // HINT: ACPI table signatures are
    //       not always as their names

    // TODO(e-kutovoi): Should this be volatile?

    static FADT *fadt = NULL;

    if (fadt == NULL) {
        fadt = (FADT*)acpi_find_table("FACP");
    }

    return fadt;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)

    return (HPET*)acpi_find_table("HPET");
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    assert(hpet != NULL);
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialisation */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        cprintf("hpetFemto = %lu\n", hpetFemto);
        hpetFreq = (1 * Peta) / hpetFemto;
        cprintf("HPET: Frequency = %lu.%03luMHz\n", (uint64_t)(hpetFreq / Mega), (uint64_t)(hpetFreq % Mega));
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    assert(hpetReg != NULL);

    return hpetReg->MAIN_CNT;
}

static void
enable_timer_interrupts(volatile uint64_t *conf, volatile uint64_t *comp, uint64_t comparator_value, uint8_t irq_num) {
    assert(hpetReg != NULL);

    cprintf("HPET Timer: IRQ Num:    %hhu\n", irq_num);
    cprintf("HPET Timer: Comparator: %lu\n", comparator_value);
    cprintf("HPET Timer: Size:       %s\n", *conf & HPET_TN_SIZE_CAP ? "64bits" : "32bits");

    // 1. Disable main counter as per instructions to avoid race conditions
    hpetReg->GEN_CONF &= ~HPET_ENABLE_CNF;
    // Validate that value was set
    assert(!(hpetReg->GEN_CONF & HPET_ENABLE_CNF));

    // 2. Enable LegacyReplacement mode
    // Already validated in initialization code, but do here just for the sake of it
    assert(hpetReg->GCAP_ID & HPET_LEG_RT_CAP);
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    // Validate that value was set
    assert(hpetReg->GEN_CONF | HPET_LEG_RT_CNF);

    // 3. Reset main counter
    hpetReg->MAIN_CNT = 0;
    // Validate that value was set
    assert(hpetReg->MAIN_CNT == 0);

    // 4. Validate that timer supports periodic mode
    assert(*conf & HPET_TN_PER_INT_CAP);

    // 5. Set timer to be periodic
    *conf |= HPET_TN_TYPE_CNF;
    // Validate that value was set
    assert(*conf & HPET_TN_TYPE_CNF);

    // TODO(e-kutovoi): Assuming 64bits for now
    assert(*conf & HPET_TN_SIZE_CAP);

    // 6. Enable setting timer frequency
    *conf |= HPET_TN_VAL_SET_CNF;
    // Validate that value was set
    assert(*conf & HPET_TN_VAL_SET_CNF);

    // 7. Set frequency
    // NOTE: 1   femtosecond = (1 / peta) second
    //       1   second = 1 peta (femtosecond)
    //       0.5 second = 1 peta (femtosecond) / 2
    //       main counter period = X femtosecond
    //       timer N period = main counter period * timer N comparator value
    //       timer N comparator value = timer N period / main counter period
    //
    //       let:
    //         timer N period = 0.5 second = 0.5 peta (femtosecond)
    //       
    //       therefore:
    //         timer N comparator value = (0.5 peta (femtosecond)) / X femtosecond
    //                                  = 0.5 peta / X
    *comp = comparator_value;
    // Will be 0 at first, but will get incremented by `comparator_value`
    // cprintf("Timer comp = %lu\n", *comp);
    // assert(*comp == 0);

    // Should get auto-reset to 0 by hardware
    assert(!(*conf & HPET_TN_VAL_SET_CNF));

    // 8. Enable timer
    *conf |= HPET_TN_INT_ENB_CNF;
    // Validate that value was set
    assert(*conf & HPET_TN_INT_ENB_CNF);

    // 9. Enable main counter
    hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
    // Validate that value was set
    assert(hpetReg->GEN_CONF & HPET_ENABLE_CNF);

    pic_irq_unmask(irq_num);
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */
void
hpet_enable_interrupts_tim0(void) {
    // LAB 5: Your code here

    cprintf("+Enable timer 0\n");
    enable_timer_interrupts(&hpetReg->TIM0_CONF, &hpetReg->TIM0_COMP, Peta / 2 / hpetFemto, IRQ_TIMER);
}

void
hpet_enable_interrupts_tim1(void) {
    // LAB 5: Your code here

    cprintf("+Enable timer 1\n");
    enable_timer_interrupts(&hpetReg->TIM1_CONF, &hpetReg->TIM1_COMP, (3 * (Peta / 2)) / hpetFemto, IRQ_CLOCK);
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here

    if (cpu_freq != 0) {
        return cpu_freq;
    }

    assert(hpetReg != NULL);

    // 1 millisecond - definite value we know, which guarantees drift <= 500ppm
    uint64_t need_femto = Peta / kilo;
    uint64_t need_ticks = need_femto / hpetFemto;

    uint64_t start_tick = hpet_get_main_cnt();
    uint64_t tick_diff = 0;

    uint64_t start_tsc = read_tsc();

    while (true) {
        uint64_t end_tick = hpet_get_main_cnt();
        tick_diff = end_tick - start_tick;

        if (tick_diff > need_ticks) {
            break;
        }

        asm volatile ("pause");
    }

    uint64_t end_tsc = read_tsc();
    uint64_t tsc_diff = end_tsc - start_tsc;

    uint64_t time_passed_femto = hpetFemto * tick_diff;
    uint64_t cpu_period_femto = time_passed_femto / tsc_diff;

    cpu_freq = Peta / cpu_period_femto;

    return cpu_freq;
}

uint64_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return (uint64_t)inl(fadt->PMTimerBlock);
}

bool
pmtimer_available(void) {
    FADT *fadt = get_fadt();
    return fadt->PMTimerLength == 4;
}

uint64_t
pmtimer_overflow(void) {
    FADT *fadt = get_fadt();
    bool is_32bit = fadt->Flags & (1u << 8);
    return is_32bit ? ((1ul << 32) - 1) : ((1ul << 24) - 1);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 *      can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here

    if (cpu_freq != 0) {
        return cpu_freq;
    }

    // Instructions as per https://wiki.osdev.org/ACPI_Timer

    if (!pmtimer_available()) {
        return 0;
    }

    uint64_t overflow = pmtimer_overflow();

    uint64_t timer_freq_hz = 3579545;
    uint64_t timer_period_femto = Peta / timer_freq_hz;

    // No information about timer's accuracy, let's just use 1ms as for hpet timer
    uint64_t need_femto = Peta / kilo;
    uint64_t need_ticks = need_femto / timer_period_femto;

    uint64_t start_tick = pmtimer_get_timeval();
    uint64_t prev_tick = start_tick;
    uint64_t tick_diff = 0;

    uint64_t start_tsc = read_tsc();

    while (true) {
        uint64_t end_tick = pmtimer_get_timeval();

        if (end_tick < prev_tick) {
            tick_diff += end_tick + overflow - prev_tick;
        } else {
            tick_diff += end_tick - prev_tick;
        }

        if (tick_diff > need_ticks) {
            break;
        }

        asm volatile ("pause");

        prev_tick = end_tick;
    }

    uint64_t end_tsc = read_tsc();
    uint64_t tsc_diff = end_tsc - start_tsc;

    uint64_t time_passed_femto = timer_period_femto * tick_diff;
    uint64_t cpu_period_femto = time_passed_femto / tsc_diff;

    cpu_freq = Peta / cpu_period_femto;

    return cpu_freq;
}
