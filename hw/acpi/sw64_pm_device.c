/*
 * Copyright (c) 2022 Wxiat Corporation
 * Written by Lufeifei, Min fanlei
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/acpi/acpi.h"
#include "hw/irq.h"
#include "hw/mem/pc-dimm.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "hw/sw64/pm.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"

static void sw64_pm_device_plug_cb(HotplugHandler *hotplug_dev,
                                    DeviceState *dev, Error **errp)
{
    SW64PMState *s = SW64_PM(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
	PCDIMMDevice *dimm = PC_DIMM(dev);
        s->addr = dimm->addr;
        s->length = object_property_get_uint(OBJECT(dimm), PC_DIMM_SIZE_PROP, NULL);
        s->status = SUNWAY_MEMHOTPLUG_ADD;
        s->slot = dimm->slot;

	acpi_memory_plug_cb(hotplug_dev, &s->acpi_memory_hotplug, dev, errp);
    } else {
        error_setg(errp, "virt: device plug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void sw64_pm_unplug_request_cb(HotplugHandler *hotplug_dev,
                               DeviceState *dev, Error **errp)
{
    SW64PMState *s = SW64_PM(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
	PCDIMMDevice *dimm = PC_DIMM(dev);
        s->addr = dimm->addr;
        s->slot = dimm->slot;
        s->length = object_property_get_uint(OBJECT(dimm), PC_DIMM_SIZE_PROP, NULL);
        s->status = SUNWAY_MEMHOTPLUG_REMOVE;

	acpi_memory_unplug_request_cb(hotplug_dev, &s->acpi_memory_hotplug, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void sw64_pm_unplug_cb(HotplugHandler *hotplug_dev,
                                       DeviceState *dev, Error **errp)
{
    SW64PMState *s = SW64_PM(hotplug_dev);

    if ((object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))) {
        acpi_memory_unplug_cb(&s->acpi_memory_hotplug, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void sw64_pm_send_event(AcpiDeviceIf *adev, AcpiEventStatusBits ev)
{
    SW64PMState *s = SW64_PM(adev);

    if (!(ev & ACPI_MEMORY_HOTPLUG_STATUS)) {
        /* Unknown event. Return without generating interrupt. */
        warn_report("GED: Uns:upported event %d. No irq injected", ev);
        return;
    }

    /* Trigger the event by sending an interrupt to the guest. */
    qemu_irq_pulse(s->irq);
}

static uint64_t pm_read(void *opaque, hwaddr addr, unsigned size)
{
    SW64PMState *s = (SW64PMState *)opaque;
    uint64_t ret = 0;

    switch (addr) {
    case OFFSET_START_ADDR:
	ret = s->addr;
	break;
    case OFFSET_LENGTH:
	ret = s->length;
	break;
    case OFFSET_STATUS:
	ret = s->status;
	break;
    case OFFSET_SLOT:
	ret = s->slot;
	break;
    default:
        break;
    }

    return ret;
}

static void pm_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    SW64PMState *s = (SW64PMState *)opaque;
    MemStatus *mdev;
    DeviceState *dev = NULL;
    HotplugHandler *hotplug_ctrl = NULL;
    Error *local_err = NULL;

    switch (addr) {
    case OFFSET_SLOT:
	 s->acpi_memory_hotplug.selector = val;
         mdev = &s->acpi_memory_hotplug.devs[s->acpi_memory_hotplug.selector];
         dev = DEVICE(mdev->dimm);
         hotplug_ctrl = qdev_get_hotplug_handler(dev);
         /* call pc-dimm unplug cb */
         hotplug_handler_unplug(hotplug_ctrl, dev, &local_err);
	 break;
    default:
        break;
    }
}

const MemoryRegionOps sw64_pm_hotplug_ops = {
    .read = pm_read,
    .write = pm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void sw64_pm_initfn(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    SW64PMState *s = SW64_PM(dev);
    MemoryRegion *pm_hotplug = g_new(MemoryRegion, 1);

    s->irq = qemu_allocate_irq(sw64_pm_set_irq, s, 13);

    memory_region_init_io(pm_hotplug, OBJECT(s), &sw64_pm_hotplug_ops, s,
                          "sw64_pm_hotplug", 4 * 1024 * 1024);
    memory_region_add_subregion(get_system_memory(), 0x803600000000ULL,
                                 pm_hotplug);

    if (s->acpi_memory_hotplug.is_enabled) {
        MachineState *machine = MACHINE(qdev_get_machine());
        MemHotplugState *state = &s->acpi_memory_hotplug;

        state->dev_count = machine->ram_slots;
        if (!state->dev_count) {
            return;
	}

        state->devs = g_malloc0(sizeof(*state->devs) * state->dev_count);
    }
}

static Property sw64_pm_properties[] = {
    DEFINE_PROP_BOOL("memory-hotplug-support", SW64PMState,
                     acpi_memory_hotplug.is_enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void sw64_pm_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(class);
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_CLASS(class);

    dc->desc = "SW64 PM";
    dc->props_ = sw64_pm_properties;

    hc->plug = sw64_pm_device_plug_cb;
    hc->unplug_request = sw64_pm_unplug_request_cb;
    hc->unplug = sw64_pm_unplug_cb;

    adevc->send_event = sw64_pm_send_event;
}

static const TypeInfo sw64_pm_info = {
    .name          = TYPE_SW64_PM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SW64PMState),
    .instance_init  = sw64_pm_initfn,
    .class_init    = sw64_pm_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { TYPE_ACPI_DEVICE_IF },
        { }
    }
};

static void sw64_pm_register_types(void)
{
    type_register_static(&sw64_pm_info);
}

type_init(sw64_pm_register_types)
