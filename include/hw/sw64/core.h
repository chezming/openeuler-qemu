#ifndef HW_SW64_CORE_H
#define HW_SW64_CORE_H

#include "hw/pci/pci_host.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "hw/sw64/pm.h"
#define TYPE_CORE3_BOARD "core3-board"
#define CORE3_BOARD(obj) \
    OBJECT_CHECK(BoardState, (obj), TYPE_CORE3_BOARD)

#define TYPE_CORE4_BOARD "core4-board"
#define CORE4_BOARD(obj) \
    OBJECT_CHECK(BoardState, (obj), TYPE_CORE4_BOARD)

struct SW64MachineClass {
    MachineClass parent;
};

struct SW64MachineState {
    MachineState parent;
    FWCfgState *fw_cfg;
    DeviceState *acpi_dev;
    PCIBus *bus;
    char *oem_id;
    char *oem_table_id;
    MemMapEntry *memmap;
    const int *irqmap;
};

#define TYPE_SW64_MACHINE   MACHINE_TYPE_NAME("sw64")
OBJECT_DECLARE_TYPE(SW64MachineState, SW64MachineClass, SW64_MACHINE)

struct CORE3MachineClass {
    MachineClass parent;
};

struct CORE3MachineState {
    MachineState parent;
    FWCfgState *fw_cfg;
    DeviceState *acpi_dev;
    PCIBus *bus;
    char *oem_id;
    char *oem_table_id;
    MemMapEntry *memmap;
    const int *irqmap;
};

#define TYPE_CORE3_MACHINE   MACHINE_TYPE_NAME("core3")
OBJECT_DECLARE_TYPE(CORE3MachineState, CORE3MachineClass, CORE3_MACHINE)

struct CORE4MachineClass {
    MachineClass parent;
};

struct CORE4MachineState {
    MachineState parent;
    FWCfgState *fw_cfg;
    DeviceState *acpi_dev;
    PCIBus *bus;
    char *oem_id;
    char *oem_table_id;
    MemMapEntry *memmap;
    const int *irqmap;
};

#define TYPE_CORE4_MACHINE   MACHINE_TYPE_NAME("core4")
OBJECT_DECLARE_TYPE(CORE4MachineState, CORE4MachineClass, CORE4_MACHINE)

typedef struct BoardState {
    PCIHostState parent_obj;
    MemoryRegion io_mcu;
    MemoryRegion io_spbu;
    MemoryRegion io_intpu;
    MemoryRegion msi_ep;
    MemoryRegion mem_ep;
    MemoryRegion mem_ep64;
    MemoryRegion conf_piu0;
    MemoryRegion io_piu0;
    MemoryRegion io_ep;
    MemoryRegion io_rtc;
    qemu_irq serial_irq;
} BoardState;

typedef struct TimerState {
    void *opaque;
    int order;
} TimerState;

enum {
    VIRT_PCIE_PIO,
    VIRT_PCIE_MMIO,
    VIRT_PCIE_CFG,
    VIRT_HIGH_PCIE_MMIO,
};

typedef struct boot_params {
    unsigned long initrd_start;                     /* logical address of initrd */
    unsigned long initrd_size;                      /* size of initrd */
    unsigned long dtb_start;                        /* logical address of dtb */
    unsigned long efi_systab;                       /* logical address of EFI system table */
    unsigned long efi_memmap;                       /* logical address of EFI memory map */
    unsigned long efi_memmap_size;                  /* size of EFI memory map */
    unsigned long efi_memdesc_size;                 /* size of an EFI memory map descriptor */
    unsigned long efi_memdesc_version;              /* memory descriptor version */
    unsigned long cmdline;                          /* logical address of cmdline */
} BOOT_PARAMS;

void core3_board_init(MachineState *machine);
void core4_board_init(MachineState *machine);
void sw64_acpi_setup(SW64MachineState *vms);
#endif
