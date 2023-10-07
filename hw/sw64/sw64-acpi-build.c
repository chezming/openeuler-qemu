/* Support for generating ACPI tables and passing them to Guests
 *
 * SW64 ACPI generation
 *
 * Copyright (C) 2023 Wang Yuanheng <wangyuanheng@wxiat.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "hw/core/cpu.h"
#include "target/sw64/cpu.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/acpi/bios-linker-loader.h"
#include "hw/acpi/aml-build.h"
#include "hw/acpi/utils.h"
#include "hw/acpi/pci.h"
#include "hw/acpi/memory_hotplug.h"
#include "hw/acpi/generic_event_device.h"
#include "hw/acpi/tpm.h"
#include "hw/pci/pcie_host.h"
#include "hw/acpi/aml-build.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/gpex.h"
#include "hw/sw64/core.h"
#include "hw/platform-bus.h"
#include "sysemu/numa.h"
#include "sysemu/reset.h"
#include "sysemu/tpm.h"
#include "kvm_sw64.h"
#include "migration/vmstate.h"
#include "hw/acpi/ghes.h"
#define SW64_PCIE_IRQMAP 16
#define ACPI_BUILD_TABLE_SIZE             0x20000
static void acpi_dsdt_add_pci_osc(Aml *dev)
{
    Aml *method, *UUID, *ifctx, *ifctx1, *elsectx;

    /* Declare an _OSC (OS Control Handoff) method */
    aml_append(dev, aml_name_decl("SUPP", aml_int(0)));
    aml_append(dev, aml_name_decl("CTRL", aml_int(0)));
    method = aml_method("_OSC", 4, AML_NOTSERIALIZED);
    aml_append(method,
        aml_create_dword_field(aml_arg(3), aml_int(0), "CDW1"));

    /* PCI Firmware Specification 3.0
     * 4.5.1. _OSC Interface for PCI Host Bridge Devices
     * The _OSC interface for a PCI/PCI-X/PCI Express hierarchy is
     * identified by the Universal Unique IDentifier (UUID)
     * 33DB4D5B-1FF7-401C-9657-7441C03DD766
     */
    UUID = aml_touuid("33DB4D5B-1FF7-401C-9657-7441C03DD766");
    ifctx = aml_if(aml_equal(aml_arg(0), UUID));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(4), "CDW2"));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(8), "CDW3"));
    aml_append(ifctx, aml_store(aml_name("CDW2"), aml_name("SUPP")));
    aml_append(ifctx, aml_store(aml_name("CDW3"), aml_name("CTRL")));
    /*
     * Allow OS control for all 5 features:
     * PCIeHotplug SHPCHotplug PME AER PCIeCapability.
     */
    aml_append(ifctx, aml_and(aml_name("CTRL"), aml_int(0x1F),
                              aml_name("CTRL")));

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_arg(1), aml_int(0x1))));
    aml_append(ifctx1, aml_or(aml_name("CDW1"), aml_int(0x08),
                              aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_name("CDW3"), aml_name("CTRL"))));
    aml_append(ifctx1, aml_or(aml_name("CDW1"), aml_int(0x10),
                              aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    aml_append(ifctx, aml_store(aml_name("CTRL"), aml_name("CDW3")));
    aml_append(ifctx, aml_return(aml_arg(3)));
    aml_append(method, ifctx);

    elsectx = aml_else();
    aml_append(elsectx, aml_or(aml_name("CDW1"), aml_int(4),
                               aml_name("CDW1")));
    aml_append(elsectx, aml_return(aml_arg(3)));
    aml_append(method, elsectx);
    aml_append(dev, method);
}

static void acpi_dsdt_add_pci(Aml *scope, uint32_t irq, SW64MachineState *vms)
{
    CrsRangeEntry *entry;
    Aml *dev, *rbuf, *method, *crs;
    CrsRangeSet crs_range_set;
    int i;
    struct GPEXConfig cfg = {
        .mmio32 = vms->memmap[VIRT_PCIE_MMIO],
	.mmio64 = vms->memmap[VIRT_HIGH_PCIE_MMIO],
        .pio    = vms->memmap[VIRT_PCIE_PIO],
        .ecam   = vms->memmap[VIRT_PCIE_CFG],
        .irq    = irq,
        .bus    = vms->bus,
    };
//PCI0
    dev = aml_device("PCI0");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A08")));
    aml_append(dev, aml_name_decl("_CID", aml_eisaid("PNP0A03")));
    aml_append(dev, aml_name_decl("_SEG", aml_int(0)));
    aml_append(dev, aml_name_decl("_BBN", aml_int(0)));
    aml_append(dev, aml_name_decl("MEMH", aml_int(0x8800)));
    aml_append(dev, aml_name_decl("NODE", aml_int(0)));
    aml_append(dev, aml_name_decl("INDX", aml_int(0)));
    aml_append(dev, aml_name_decl("RCCB", aml_int(0x880500000000)));
    aml_append(dev, aml_name_decl("RCIO", aml_int(0x880000000000)));
    aml_append(dev, aml_name_decl("EPIO", aml_int(0x880100000000)));
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, method);
    rbuf = aml_resource_template();
    aml_append(dev, aml_name_decl("CRS0", rbuf));
    aml_append(rbuf,
        aml_word_bus_number(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                            0x0000, 0x0000, 0xFF, 0x0000, 0x100));
    crs_range_set_init(&crs_range_set);
    if (cfg.mmio32.size) {
        crs_replace_with_free_ranges(crs_range_set.mem_ranges,
                                     cfg.mmio32.base,
                                     cfg.mmio32.base + cfg.mmio32.size - 1);
        for (i = 0; i < crs_range_set.mem_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.mem_ranges, i);
            aml_append(rbuf,
                aml_dword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                                 AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                                 entry->base, entry->limit,
                                 0x0000, entry->limit - entry->base + 1));
        }
    }

    if (cfg.pio.size) {
        crs_replace_with_free_ranges(crs_range_set.io_ranges,
                                     cfg.pio.base,
                                     cfg.pio.base + cfg.pio.size - 1);
        for (i = 0; i < crs_range_set.io_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.io_ranges, i);
            aml_append(rbuf,
                aml_qword_io(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                             AML_ENTIRE_RANGE, 0x0000, entry->base,
                             entry->limit, 0x0000,
                             entry->limit - entry->base + 1));
        }
    }
    if (cfg.mmio64.size) {
        crs_replace_with_free_ranges(crs_range_set.mem_64bit_ranges,
                                     cfg.mmio64.base,
                                     cfg.mmio64.base + cfg.mmio64.size - 1);
        for (i = 0; i < crs_range_set.mem_64bit_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.mem_64bit_ranges, i);
            aml_append(rbuf,
                aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                                 AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                                 entry->base,
                                 entry->limit, 0x0000,
                                 entry->limit - entry->base + 1));
        }
    }

    method = aml_method("_CRS", 0, AML_SERIALIZED);
    aml_append(method, aml_return(rbuf));
    aml_append(dev, method);
    acpi_dsdt_add_pci_osc(dev);
//RES0
    Aml *dev_res0 = aml_device("%s", "RES0");
    aml_append(dev_res0, aml_name_decl("_HID", aml_string("PNP0C02")));
    crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                         cfg.ecam.base,
                         cfg.ecam.base + cfg.ecam.size - 1,
                         0x0000,
                         cfg.ecam.size));
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, dev_res0);
    aml_append(scope, dev);
    crs_range_set_free(&crs_range_set);
}

static void
build_madt(GArray *table_data, BIOSLinker *linker, SW64MachineState *vms)
{
}

/* DSDT */
static void
build_dsdt(GArray *table_data, BIOSLinker *linker, SW64MachineState *vms)
{
    Aml *scope, *dsdt;
    AcpiTable table = { .sig = "DSDT", .rev = 2, .oem_id = vms->oem_id,
                        .oem_table_id = vms->oem_table_id };

    acpi_table_begin(&table, table_data);
    dsdt = init_aml_allocator();

    /* When booting the VM with UEFI, UEFI takes ownership of the RTC hardware.
     * While UEFI can use libfdt to disable the RTC device node in the DTB that
     * it passes to the OS, it cannot modify AML. Therefore, we won't generate
     * the RTC ACPI device at all when using UEFI.
     */
    scope = aml_scope("\\_SB");
    acpi_dsdt_add_pci(scope, SW64_PCIE_IRQMAP, vms);

    aml_append(dsdt, scope);

    /* copy AML table into ACPI tables blob */
    g_array_append_vals(table_data, dsdt->buf->data, dsdt->buf->len);

    acpi_table_end(linker, &table);
    free_aml_allocator();
}

typedef
struct AcpiBuildState {
    /* Copy of table in RAM (for patching). */
    MemoryRegion *table_mr;
    MemoryRegion *rsdp_mr;
    MemoryRegion *linker_mr;
    /* Is table patched? */
    bool patched;
} AcpiBuildState;

static void acpi_align_size(GArray *blob, unsigned align)
{
    /*
     * Align size to multiple of given size. This reduces the chance
     * we need to change size in the future (breaking cross version migration).
     */
    g_array_set_size(blob, ROUND_UP(acpi_data_len(blob), align));
}

static
void sw64_acpi_build(SW64MachineState *vms, AcpiBuildTables *tables)
{
    GArray *table_offsets;
    unsigned dsdt, xsdt;
    GArray *tables_blob = tables->table_data;

    table_offsets = g_array_new(false, true /* clear */,
                                        sizeof(uint32_t));

    bios_linker_loader_alloc(tables->linker,
                             ACPI_BUILD_TABLE_FILE, tables_blob,
                             64, false /* high memory */);

    /* DSDT is pointed to by FADT */
    dsdt = tables_blob->len;
    build_dsdt(tables_blob, tables->linker, vms);

    /* FADT MADT MCFG is pointed to by XSDT*/
    acpi_add_table(table_offsets, tables_blob);
    {
        AcpiFadtData fadt = {
            .rev = 5,
            .minor_ver = 1,
            .flags = 1 << ACPI_FADT_F_HW_REDUCED_ACPI,
            .xdsdt_tbl_offset = &dsdt,
        };
        build_fadt(tables_blob, tables->linker, &fadt, vms->oem_id, vms->oem_table_id);
    }

    acpi_add_table(table_offsets, tables_blob);
    build_madt(tables_blob, tables->linker, vms);

    acpi_add_table(table_offsets, tables_blob);
    {
        AcpiMcfgInfo mcfg = {
           .base = vms->memmap[VIRT_PCIE_CFG].base,
           .size = vms->memmap[VIRT_PCIE_CFG].size,
        };
        build_mcfg(tables_blob, tables->linker, &mcfg, vms->oem_id,
                   vms->oem_table_id);
    }
    /* XSDT is pointed to by RSDP */
    xsdt = tables_blob->len;
    build_xsdt(tables_blob, tables->linker, table_offsets, vms->oem_id,
               vms->oem_table_id);

    /* RSDP is in FSEG memory, so allocate it separately */
    {
        AcpiRsdpData rsdp_data = {
            .revision = 2,
            .oem_id = vms->oem_id,
            .xsdt_tbl_offset = &xsdt,
            .rsdt_tbl_offset = NULL,
        };
        build_rsdp(tables->rsdp, tables->linker, &rsdp_data);
    }

    /*
     * The align size is 128, warn if 64k is not enough therefore
     * the align size could be resized.
     */
    if (tables_blob->len > ACPI_BUILD_TABLE_SIZE / 2) {
        warn_report("ACPI table size %u exceeds %d bytes,"
                    " migration may not work",
                    tables_blob->len, ACPI_BUILD_TABLE_SIZE / 2);
        error_printf("Try removing CPUs, NUMA nodes, memory slots"
                     " or PCI bridges.");
    }
    acpi_align_size(tables_blob, ACPI_BUILD_TABLE_SIZE);


    /* Cleanup memory that's no longer used. */
    g_array_free(table_offsets, true);
}

static void acpi_ram_update(MemoryRegion *mr, GArray *data)
{
    uint32_t size = acpi_data_len(data);

    /* Make sure RAM size is correct - in case it got changed
     * e.g. by migration */
    memory_region_ram_resize(mr, size, &error_abort);

    memcpy(memory_region_get_ram_ptr(mr), data->data, size);
    memory_region_set_dirty(mr, 0, size);
}

static void sw64_acpi_build_update(void *build_opaque)
{
    AcpiBuildState *build_state = build_opaque;
    AcpiBuildTables tables;

    /* No state to update or already patched? Nothing to do. */
    if (!build_state || build_state->patched) {
        return;
    }
    build_state->patched = true;

    acpi_build_tables_init(&tables);

    sw64_acpi_build((SW64MachineState*)CORE3_MACHINE(qdev_get_machine()), &tables);

    acpi_ram_update(build_state->table_mr, tables.table_data);
    acpi_ram_update(build_state->rsdp_mr, tables.rsdp);
    acpi_ram_update(build_state->linker_mr, tables.linker->cmd_blob);

    acpi_build_tables_cleanup(&tables, true);
}

static void sw64_acpi_build_reset(void *build_opaque)
{
    AcpiBuildState *build_state = build_opaque;
    build_state->patched = false;
}

static const VMStateDescription vmstate_sw64_acpi_build = {
    .name = "sw64_acpi_build",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(patched, AcpiBuildState),
        VMSTATE_END_OF_LIST()
    },
};

void sw64_acpi_setup(SW64MachineState *vms)
{
    AcpiBuildTables tables;
    AcpiBuildState *build_state;

    if (!vms->fw_cfg) {
        return;
    }

    build_state = g_malloc0(sizeof *build_state);

    acpi_build_tables_init(&tables);

    sw64_acpi_build(vms, &tables);
    /* Now expose it all to Guest */
    build_state->table_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                              build_state, tables.table_data,
                                              ACPI_BUILD_TABLE_FILE);
    assert(build_state->table_mr != NULL);

    build_state->linker_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                               build_state,
                                               tables.linker->cmd_blob,
                                               ACPI_BUILD_LOADER_FILE);
    build_state->rsdp_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                             build_state, tables.rsdp,
                                             ACPI_BUILD_RSDP_FILE);

    qemu_register_reset(sw64_acpi_build_reset, build_state);
    sw64_acpi_build_reset(build_state);
    vmstate_register(NULL, 0, &vmstate_sw64_acpi_build, build_state);

    /* Cleanup tables but don't free the memory: we track it
     * in build_state.
     */
    acpi_build_tables_cleanup(&tables, false);
}
