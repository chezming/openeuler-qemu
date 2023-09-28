#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sw64/core.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "net/net.h"
#include "hw/usb.h"
#include "hw/ide/pci.h"
#include "hw/ide/ahci.h"
#include "sysemu/numa.h"
#include "sysemu/kvm.h"
#include "sysemu/cpus.h"
#include "hw/pci/msi.h"
#include "hw/sw64/sw64_iommu.h"
#include "hw/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/boards.h"
#include "sysemu/kvm.h"
#include "hw/firmware/smbios.h"

#define CORE3_MAX_CPUS_MASK		0x3ff
#define CORE3_CORES_SHIFT		10
#define CORE3_CORES_MASK		0x3ff
#define CORE3_THREADS_SHIFT		20
#define CORE3_THREADS_MASK		0xfff

#define MAX_IDE_BUS 2
#define SW_PIN_TO_IRQ 16

#define SW_FW_CFG_P_BASE (0x804920000000ULL)

static FWCfgState *sw_create_fw_cfg(hwaddr addr)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    uint16_t smp_cpus = ms->smp.cpus;
    FWCfgState *fw_cfg;
    fw_cfg = fw_cfg_init_mem_wide(addr + 8, addr, 8,
			     addr + 16, &address_space_memory);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, smp_cpus);
    return fw_cfg;
}

#ifdef CONFIG_KVM
static void sw64_virt_build_smbios(CORE3MachineState *swms)
{
    uint8_t *smbios_tables, *smbios_anchor;
    size_t smbios_tables_len, smbios_anchor_len;
    const char *product = "QEMU Virtual Machine";

    if (!swms->fw_cfg) {
        return;
    }

    if (kvm_enabled()) {
        product = "KVM Virtual Machine";
    }
    smbios_set_defaults("QEMU", product,
                        "sw64", false,
                        true, SMBIOS_ENTRY_POINT_30);


    smbios_get_tables(MACHINE(qdev_get_machine()), NULL, 0,
		      &smbios_tables, &smbios_tables_len,
                      &smbios_anchor, &smbios_anchor_len,
		      &error_fatal);

    if (smbios_anchor) {
        fw_cfg_add_file(swms->fw_cfg, "etc/smbios/smbios-tables",
                        smbios_tables, smbios_tables_len);
        fw_cfg_add_file(swms->fw_cfg, "etc/smbios/smbios-anchor",
                        smbios_anchor, smbios_anchor_len);
    }
}
#endif

#ifndef CONFIG_KVM
static void swboard_alarm_timer(void *opaque)
{
    TimerState *ts = (TimerState *)((uintptr_t)opaque);

    int cpu = ts->order;
    cpu_interrupt(qemu_get_cpu(cpu), CPU_INTERRUPT_TIMER);
}
#endif

static PCIINTxRoute sw_route_intx_pin_to_irq(void *opaque, int pin)
{
       PCIINTxRoute route;

       route.mode = PCI_INTX_ENABLED;
       route.irq = SW_PIN_TO_IRQ;
       return route;
}

static uint64_t convert_bit(int n)
{
    uint64_t ret;

    if (n == 64)
        ret = 0xffffffffffffffffUL;
    else
        ret = (1UL << n) - 1;

    return ret;
}

static uint64_t mcu_read(void *opaque, hwaddr addr, unsigned size)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned int smp_cpus = ms->smp.cpus;
    unsigned int smp_threads = ms->smp.threads;
    unsigned int smp_cores = ms->smp.cores;
    unsigned int max_cpus = ms->smp.max_cpus;
    uint64_t ret = 0;
    switch (addr) {
    case 0x0000:
    /* CG_ONLINE */
        {
            int i;
            for (i = 0; i < smp_cpus; i = i + 4)
                ret |= (1UL << i);
        }
        break;
    case 0x0080:
    /* SMP_INFO */
	{
	    ret = (smp_threads & CORE3_THREADS_MASK) << CORE3_THREADS_SHIFT;
	    ret += (smp_cores & CORE3_CORES_MASK) << CORE3_CORES_SHIFT;
	    ret += max_cpus & CORE3_MAX_CPUS_MASK;
	}
	break;
    /*IO_START*/
    case 0x1300:
        ret = 0x1;
        break;
    case 0x3780:
    /* MC_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    case 0x0900:
    /* CPUID */
        ret = 0;
        break;
    case 0x1180:
    /* LONGTIME */
        ret = qemu_clock_get_ns(QEMU_CLOCK_HOST) / 80;
        break;
    case 0x4900:
    /* MC_CONFIG */
        break;
    case 0x0780:
    /* CORE_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    case 0x0680:
    /* INIT_CTL */
        ret = 0x000003AE00000D28;
        break;
    default:
        fprintf(stderr, "Unsupported MCU addr: 0x%04lx\n", addr);
        return -1;
    }
    return ret;
}

static void mcu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
#ifndef CONFIG_KVM
#ifdef CONFIG_DUMP_PRINTK
    uint64_t print_addr;
    uint32_t len;
    int i;

    if (addr == 0x40000) {
        print_addr = val & 0x7fffffff;
        len = (uint32_t)(val >> 32);
        uint8_t *buf;
        buf = malloc(len + 10);
        memset(buf, 0, len + 10);
        cpu_physical_memory_rw(print_addr, buf, len, 0);
        for (i = 0; i < len; i++)
            printf("%c", buf[i]);

        free(buf);
        return;
    }
#endif
#endif
}

static const MemoryRegionOps mcu_ops = {
    .read = mcu_read,
    .write = mcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
};

static uint64_t intpu_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t ret = 0;
#ifndef CONFIG_KVM
    switch (addr) {
    case 0x180:
    /* LONGTIME */
        ret = qemu_clock_get_ns(QEMU_CLOCK_HOST) / 32;
        break;
    }
#endif
    return ret;
}

static void intpu_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
#ifndef CONFIG_KVM
    SW64CPU *cpu_current = SW64_CPU(current_cpu);
    switch (addr) {
    case 0x00:
        cpu_interrupt(qemu_get_cpu(val&0x3f), CPU_INTERRUPT_II0);
        cpu_current->env.csr[II_REQ] &= ~(1 << 20);
        break;
    default:
        fprintf(stderr, "Unsupported IPU addr: 0x%04lx\n", addr);
        break;
    }
#endif
}

static const MemoryRegionOps intpu_ops = {
    .read = intpu_read,
    .write = intpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
};

static MemTxResult msi_read(void *opaque, hwaddr addr,
				uint64_t *data, unsigned size,
				MemTxAttrs attrs)
{
     return MEMTX_OK;
}

MemTxResult msi_write(void *opaque, hwaddr addr,
                      uint64_t value, unsigned size,
                      MemTxAttrs attrs)
{
#ifdef CONFIG_KVM
    int ret = 0;
    MSIMessage msg = {};

    msg.address = (uint64_t) addr + 0x8000fee00000;
    msg.data = (uint32_t) value;

    ret = kvm_irqchip_send_msi(kvm_state, msg);
    if (ret < 0) {
        fprintf(stderr, "KVM: injection failed, MSI lost (%s)\n",
                strerror(-ret));
    }
#endif
    return MEMTX_OK;
}

static const MemoryRegionOps msi_ops = {
   .read_with_attrs = msi_read,
   .write_with_attrs = msi_write,
   .endianness = DEVICE_LITTLE_ENDIAN,
   .valid =
       {
           .min_access_size = 1,
           .max_access_size = 8,
       },
   .impl =
       {
           .min_access_size = 1,
           .max_access_size = 8,
       },
};

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = get_clock_realtime() / NANOSECONDS_PER_SECOND;
    return val;
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t val,
                      unsigned size)
{
}

static const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
};

static uint64_t ignore_read(void *opaque, hwaddr addr, unsigned size)
{
    return 1;
}

static void ignore_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

const MemoryRegionOps core3_pci_ignore_ops = {
    .read = ignore_read,
    .write = ignore_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
};

static uint64_t config_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIBus *b = opaque;
    uint32_t trans_addr = 0;
    trans_addr |= ((addr >> 16) & 0xffff) << 8;
    trans_addr |= (addr & 0xff);
    return pci_data_read(b, trans_addr, size);
}

static void config_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    PCIBus *b = opaque;
    uint32_t trans_addr = 0;
    trans_addr |= ((addr >> 16) & 0xffff) << 8;
    trans_addr |= (addr & 0xff);
    pci_data_write(b, trans_addr, val, size);
}

const MemoryRegionOps core3_pci_config_ops = {
    .read = config_read,
    .write = config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
};

static void swboard_set_irq(void *opaque, int irq, int level)
{
    if (level == 0)
        return;

    if (kvm_enabled()) {
        kvm_set_irq(kvm_state, irq, level);
        return;
    }

    cpu_interrupt(qemu_get_cpu(0), CPU_INTERRUPT_PCIE);
}

static int swboard_map_irq(PCIDevice *d, int irq_num)
{
    /* In fact,the return value is the interrupt type passed to kernel,
     * so it must keep same with the type in do_entInt in kernel.
     */
    return 16;
}

static void serial_set_irq(void *opaque, int irq, int level)
{
    if (level == 0)
        return;
    if (kvm_enabled()) {
        kvm_set_irq(kvm_state, irq, level);
        return;
    }
    cpu_interrupt(qemu_get_cpu(0), CPU_INTERRUPT_HARD);
}

static void sw64_new_cpu(const char *typename, int64_t arch_id, Error **errp)
{
    Object *cpu = NULL;
    Error *local_err = NULL;

    cpu = object_new(typename);
    object_property_set_uint(cpu, "cid", arch_id, &local_err);
    object_property_set_bool(cpu, "realized", true, &local_err);

    object_unref(cpu);
    error_propagate(errp, local_err);
}

static void core3_cpus_init(MachineState *ms)
{
    int i;
    const CPUArchIdList *possible_cpus;
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    possible_cpus = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < ms->smp.cpus; i++) {
            sw64_new_cpu("core3-sw64-cpu", possible_cpus->cpus[i].arch_id, &error_fatal);
    }
}

void core3_board_init(MachineState *ms)
{
    CORE3MachineState *swms = CORE3_MACHINE(ms);
    DeviceState *dev;
    BoardState *bs;
    uint64_t MB = 1024 * 1024;
    uint64_t GB = 1024 * MB;
    int i;
    PCIBus *b;
    PCIHostState *phb;

    core3_cpus_init(ms);

    dev = qdev_new(TYPE_CORE3_BOARD);
    bs = CORE3_BOARD(dev);
    phb = PCI_HOST_BRIDGE(dev);

#ifdef CONFIG_KVM
    if (kvm_has_gsi_routing())
        msi_nonbroken = true;
#endif

#ifndef CONFIG_KVM
    TimerState *ts;
    SW64CPU *cpu;
    for (i = 0; i < ms->smp.cpus; ++i) {
        cpu = SW64_CPU(qemu_get_cpu(i));
        ts = g_new(TimerState, 1);
        ts->opaque = (void *) ((uintptr_t)bs);
        ts->order = i;
        cpu->alarm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &swboard_alarm_timer, ts);
    }
#endif

    memory_region_add_subregion(get_system_memory(), 0, ms->ram);

    memory_region_init_io(&bs->io_mcu, NULL, &mcu_ops, bs, "io_mcu", 16 * MB);
    memory_region_add_subregion(get_system_memory(), 0x803000000000ULL, &bs->io_mcu);

    memory_region_init_io(&bs->io_intpu, NULL, &intpu_ops, bs, "io_intpu", 1 * MB);
    memory_region_add_subregion(get_system_memory(), 0x802a00000000ULL,
                                &bs->io_intpu);

    memory_region_init_io(&bs->msi_ep, NULL, &msi_ops, bs, "msi_ep", 1 * MB);
    memory_region_add_subregion(get_system_memory(), 0x8000fee00000ULL, &bs->msi_ep);

    memory_region_init(&bs->mem_ep, OBJECT(bs), "pci0-mem", 0x890000000000ULL);
    memory_region_add_subregion(get_system_memory(), 0x880000000000ULL, &bs->mem_ep);

    memory_region_init_alias(&bs->mem_ep64, NULL, "mem_ep64", &bs->mem_ep, 0x888000000000ULL, 1ULL << 39);
    memory_region_add_subregion(get_system_memory(), 0x888000000000ULL, &bs->mem_ep64);

    memory_region_init_io(&bs->io_ep, OBJECT(bs), &core3_pci_ignore_ops, NULL,
                          "pci0-io-ep", 4 * GB);

    memory_region_add_subregion(get_system_memory(), 0x880100000000ULL, &bs->io_ep);
    b = pci_register_root_bus(dev, "pcie.0", swboard_set_irq, swboard_map_irq, bs,
                         &bs->mem_ep, &bs->io_ep, 0, 537, TYPE_PCIE_BUS);
    phb->bus = b;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    pci_bus_set_route_irq_fn(b, sw_route_intx_pin_to_irq);
    memory_region_init_io(&bs->conf_piu0, OBJECT(bs), &core3_pci_config_ops, b,
                          "pci0-ep-conf-io", 4 * GB);
    memory_region_add_subregion(get_system_memory(), 0x880600000000ULL,
                                &bs->conf_piu0);
    memory_region_init_io(&bs->io_rtc, OBJECT(bs), &rtc_ops, b,
                          "sw64-rtc", 0x08ULL);
    memory_region_add_subregion(get_system_memory(), 0x804910000000ULL,
                                &bs->io_rtc);
#ifdef CONFIG_SW64_VT_IOMMU
    sw64_vt_iommu_init(b);
#endif
    for (i = 0; i < nb_nics; i++) {
        pci_nic_init_nofail(&nd_table[i], b, "e1000", NULL);
    }

    pci_vga_init(b);
#define MAX_SATA_PORTS 6
    PCIDevice *ahci;
    DriveInfo *hd[MAX_SATA_PORTS];
    ahci = pci_create_simple_multifunction(b, PCI_DEVFN(0x1f, 0), true,
                                TYPE_ICH9_AHCI);
    g_assert(MAX_SATA_PORTS == ahci_get_num_ports(ahci));
    ide_drive_get(hd, ahci_get_num_ports(ahci));
    ahci_ide_create_devs(ahci, hd);

    bs->serial_irq = qemu_allocate_irq(serial_set_irq, bs, 12);
    if (serial_hd(0)) {
        serial_mm_init(get_system_memory(), 0x3F8 + 0x880100000000ULL, 0,
                       bs->serial_irq, (1843200 >> 4), serial_hd(0),
                       DEVICE_LITTLE_ENDIAN);
    }
    pci_create_simple(phb->bus, -1, "nec-usb-xhci");
    swms->fw_cfg = sw_create_fw_cfg(SW_FW_CFG_P_BASE);
    rom_set_fw(swms->fw_cfg);
#ifdef CONFIG_KVM
    sw64_virt_build_smbios(swms);
#endif
}

static const TypeInfo swboard_pcihost_info = {
    .name = TYPE_CORE3_BOARD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(BoardState),
};

static void swboard_register_types(void)
{
    type_register_static(&swboard_pcihost_info);
}

type_init(swboard_register_types)
