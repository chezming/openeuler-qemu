#ifndef HW_SW64_PM_H
#define HW_SW64_PM_H

#include "hw/sysbus.h"
#include "hw/acpi/memory_hotplug.h"

#define OFFSET_START_ADDR       0x0
#define OFFSET_LENGTH           0x8
#define OFFSET_STATUS           0x10
#define OFFSET_SLOT             0x18

#define SUNWAY_MEMHOTPLUG_ADD      0x1
#define SUNWAY_MEMHOTPLUG_REMOVE   0x2

typedef struct SW64PMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    qemu_irq irq;
    MemHotplugState acpi_memory_hotplug;
    unsigned long addr;
    unsigned long length;
    unsigned long status;
    unsigned long slot;
} SW64PMState;

#define TYPE_SW64_PM "SW64_PM"

DECLARE_INSTANCE_CHECKER(SW64PMState, SW64_PM, TYPE_SW64_PM)

void sw64_pm_set_irq(void *opaque, int irq, int level);
#endif
