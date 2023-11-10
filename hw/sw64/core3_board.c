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
#include "hw/sw64/sunway.h"
#include "hw/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/firmware/smbios.h"
#include "sysemu/device_tree.h"
#include "qemu/datadir.h"

#define CORE3_MAX_CPUS_MASK		0x3ff
#define CORE3_CORES_SHIFT		10
#define CORE3_CORES_MASK		0x3ff
#define CORE3_THREADS_SHIFT		20
#define CORE3_THREADS_MASK		0xfff

#define MAX_IDE_BUS 2
#define SW_FW_CFG_P_BASE (0x804920000000ULL)

static const MemMapEntry base_memmap[] = {
    [VIRT_PCIE_PIO] =           { 0x880100000000, 0x100000000 },
    [VIRT_PCIE_MMIO] =          {     0xe0000000,  0x20000000 },
    [VIRT_PCIE_CFG] =           { 0x880600000000,  0x10000000 },
    [VIRT_HIGH_PCIE_MMIO] =     { 0x888000000000, 0x8000000000 },
};

static void core3_virt_build_smbios(CORE3MachineState *core3ms)
{
    FWCfgState *fw_cfg = core3ms->fw_cfg;

    if (!fw_cfg)
        return;

    sw64_virt_build_smbios(fw_cfg);
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
    case 0x0080:
    /* SMP_INFO */
	{
	    ret = (smp_threads & CORE3_THREADS_MASK) << CORE3_THREADS_SHIFT;
	    ret += (smp_cores & CORE3_CORES_MASK) << CORE3_CORES_SHIFT;
	    ret += max_cpus & CORE3_MAX_CPUS_MASK;
	}
	break;
    case 0x0780:
    /* CORE_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    case 0x3780:
    /* MC_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    default:
        fprintf(stderr, "Unsupported MCU addr: 0x%04lx\n", addr);
        return -1;
    }
    return ret;
}

static void mcu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
#ifdef CONFIG_DUMP_PRINTK
    uint64_t print_addr;
    uint32_t len;
    int i;

    if (kvm_enabled())
	return;

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

    if (kvm_enabled())
	return ret;

    switch (addr) {
    case 0x180:
    /* LONGTIME */
        ret = qemu_clock_get_ns(QEMU_CLOCK_HOST) / 32;
        break;
    }
    return ret;
}

static void intpu_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    SW64CPU *cpu_current = SW64_CPU(current_cpu);

    if (kvm_enabled())
	return;

    switch (addr) {
    case 0x00:
        cpu_interrupt(qemu_get_cpu(val & 0x3f), CPU_INTERRUPT_II0);
        cpu_current->env.csr[II_REQ] &= ~(1 << 20);
        break;
    default:
        fprintf(stderr, "Unsupported IPU addr: 0x%04lx\n", addr);
        break;
    }
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

static void core3_create_fdt(CORE3MachineState *c3ms)
{
    uint32_t intc_phandle;
    MachineState *ms = MACHINE(c3ms);

    if (ms->dtb) {
        char *filename;

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->dtb);
        if (!filename) {
            fprintf(stderr, "Couldn't open dtb file %s\n", ms->dtb);
            exit(1);
        }

        ms->fdt = load_device_tree(ms->dtb, &c3ms->fdt_size);
        if (!ms->fdt) {
            error_report("load_device_tree() failed");
            exit(1);
        }
    } else {
        ms->fdt = create_device_tree(&c3ms->fdt_size);
        if (!ms->fdt) {
            error_report("create_device_tree() failed");
            exit(1);
        }

        qemu_fdt_setprop_string(ms->fdt, "/", "compatible", "sunway,chip3");
        qemu_fdt_setprop_string(ms->fdt, "/", "model", "chip3");
        qemu_fdt_setprop_cell(ms->fdt, "/", "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/", "#size-cells", 0x2);

        qemu_fdt_add_subnode(ms->fdt, "/soc");
        qemu_fdt_setprop_string(ms->fdt, "/soc", "compatible", "simple-bus");
        qemu_fdt_setprop_cell(ms->fdt, "/soc", "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc", "#size-cells", 0x2);
        qemu_fdt_setprop(ms->fdt, "/soc", "ranges", NULL, 0);

        intc_phandle = qemu_fdt_alloc_phandle(ms->fdt);
        qemu_fdt_add_subnode(ms->fdt, "/soc/interrupt-controller");
        qemu_fdt_setprop_string(ms->fdt, "/soc/interrupt-controller",
                                "compatible", "sw64,sw6_irq_vt_controller");
        qemu_fdt_setprop(ms->fdt, "/soc/interrupt-controller",
                         "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "#interrupt-cells", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "phandle", intc_phandle);

        qemu_fdt_add_subnode(ms->fdt, "/soc/serial0@8801");
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "#size-cells", 0x2);
        qemu_fdt_setprop_string(ms->fdt, "/soc/serial0@8801",
                                "compatible", "ns16550a");
        qemu_fdt_setprop_sized_cells(ms->fdt, "/soc/serial0@8801",
                                     "reg", 2, 0x8801000003f8, 2, 0x10);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "interrupt-parent", intc_phandle);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801", "interrupts", 12);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801", "reg-shift", 0x0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "reg-io-width", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "clock-frequency", 24000000);
        qemu_fdt_setprop_string(ms->fdt, "/soc/serial0@8801",
                                "status", "okay");

        qemu_fdt_add_subnode(ms->fdt, "/soc/misc0@8036");
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "#size-cells", 0x2);
        qemu_fdt_setprop_string(ms->fdt, "/soc/misc0@8036",
                                "compatible", "sw6,sunway-ged");
        qemu_fdt_setprop_sized_cells(ms->fdt, "/soc/misc0@8036",
                                     "reg", 2, 0x803600000000, 2, 0x20);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "interrupt-parent", intc_phandle);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "interrupts", 13);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "reg-shift", 0x0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "reg-io-width", 0x8);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "clock-frequency", 24000000);
        qemu_fdt_setprop_string(ms->fdt, "/soc/misc0@8036", "status", "okay");

        qemu_fdt_add_subnode(ms->fdt, "/soc/fw_cfg@8049");
        qemu_fdt_setprop_string(ms->fdt, "/soc/fw_cfg@8049",
                                "compatible", "qemu,fw-cfg-mmio");
        qemu_fdt_setprop(ms->fdt, "/soc/fw_cfg@8049", "dma-coherent", NULL, 0);
        qemu_fdt_setprop_sized_cells(ms->fdt, "/soc/fw_cfg@8049",
                                     "reg", 2, SW_FW_CFG_P_BASE, 2, 0x18);
    }
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
    CORE3MachineState *core3ms = CORE3_MACHINE(ms);
    DeviceState *dev = qdev_new(TYPE_CORE3_BOARD);
    BoardState *bs = CORE3_BOARD(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    uint64_t MB = 1024 * 1024;
    uint64_t GB = 1024 * MB;
    PCIBus *b;

    /* Create device tree */
    core3_create_fdt(core3ms);

    core3_cpus_init(ms);

    if (kvm_enabled()) {
        if (kvm_has_gsi_routing())
            msi_nonbroken = true;
    }
    else
	sw64_create_alarm_timer(ms, bs);

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

    memory_region_init_io(&bs->io_ep, OBJECT(bs), &sw64_pci_ignore_ops, NULL,
                          "pci0-io-ep", 4 * GB);
    memory_region_add_subregion(get_system_memory(), 0x880100000000ULL, &bs->io_ep);

    b = pci_register_root_bus(dev, "pcie.0", sw64_board_set_irq, sw64_board_map_irq, bs,
                         &bs->mem_ep, &bs->io_ep, 0, 537, TYPE_PCIE_BUS);
    phb->bus = b;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    pci_bus_set_route_irq_fn(b, sw64_route_intx_pin_to_irq);
    memory_region_init_io(&bs->conf_piu0, OBJECT(bs), &sw64_pci_config_ops, b,
                          "pci0-ep-conf-io", 4 * GB);
    memory_region_add_subregion(get_system_memory(), 0x880600000000ULL,
                                &bs->conf_piu0);
    memory_region_init_io(&bs->io_rtc, OBJECT(bs), &rtc_ops, b,
                          "sw64-rtc", 0x08ULL);
    memory_region_add_subregion(get_system_memory(), 0x804910000000ULL,
                                &bs->io_rtc);
    object_property_add_tm(OBJECT(core3ms), "rtc-time", rtc_get_time);
#ifdef CONFIG_SW64_VT_IOMMU
    sw64_vt_iommu_init(b);
#endif

    sw64_create_pcie(bs, b, phb);

    core3ms->fw_cfg = sw64_create_fw_cfg(SW_FW_CFG_P_BASE);
    rom_set_fw(core3ms->fw_cfg);

    core3ms->bus = phb->bus;
    core3ms->memmap = base_memmap;
    sw64_acpi_setup((SW64MachineState*)core3ms);

    core3_virt_build_smbios(core3ms);
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
