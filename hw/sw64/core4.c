/*
 * QEMU CORE4 hardware system emulator.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "cpu.h"
#include "hw/hw.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "hw/ide.h"
#include "hw/char/serial.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "hw/sw64/core.h"
#include "hw/sw64/sunway.h"
#include "hw/boards.h"
#include "sysemu/numa.h"
#include "hw/mem/pc-dimm.h"
#include "qapi/error.h"

#define MAX_CPUS_CORE4 64
#define C4_UEFI_BIOS_NAME "c4-uefi-bios-sw"

static void core4_init(MachineState *machine)
{
    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    const char *hmcode_name = kvm_enabled() ? "core4-reset":"core4-hmcode";
    const char *bios_name = C4_UEFI_BIOS_NAME;
    char *hmcode_filename;
    uint64_t hmcode_entry, kernel_entry;

    core4_board_init(machine);

    sw64_set_ram_size(ram_size);

    hmcode_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, hmcode_name);
    if (hmcode_filename == NULL) {
	error_report("no '%s' provided", hmcode_name);
        exit(1);
    }
    sw64_load_hmcode(hmcode_filename, &hmcode_entry);

    if (!kvm_enabled()) {
	CPUState *cpu;
	SW64CPU *sw64_cpu;
	CPU_FOREACH(cpu) {
	    sw64_cpu = SW64_CPU(cpu);
	    sw64_cpu->env.pc = hmcode_entry;
	    sw64_cpu->env.hm_entry = hmcode_entry;
	    sw64_cpu->env.csr[CID] = sw64_cpu->cid;
	    qemu_register_reset(sw64_cpu_reset, sw64_cpu);
	}
    }
    g_free(hmcode_filename);

    if (!kernel_filename)
        sw64_find_and_load_bios(bios_name);
    else
	sw64_load_kernel(kernel_filename, &kernel_entry, kernel_cmdline);

    if (initrd_filename) {
	sw64_load_initrd(initrd_filename);
    }
}

static HotplugHandler *sw64_get_hotplug_handler(MachineState *machine,
                                             DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))
        return HOTPLUG_HANDLER(machine);

    return NULL;
}

static void core4_machine_device_pre_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))
        pc_dimm_pre_plug(PC_DIMM(dev), ms, NULL, &local_err);
    else
	error_setg(errp, "memory hotplug is not enabled");

    return;
}

static void core4_machine_device_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))
        pc_dimm_plug(PC_DIMM(dev), ms);

    hotplug_handler_plug(HOTPLUG_HANDLER(core4ms->acpi_dev),
                          dev, &local_err);
}

static void core4_machine_device_unplug_request_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        hotplug_handler_unplug_request(HOTPLUG_HANDLER(core4ms->acpi_dev),
                          dev, &local_err);
    } else {
        error_setg(&local_err, "device unplug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void core4_machine_device_unplug_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        hotplug_handler_unplug(HOTPLUG_HANDLER(core4ms->acpi_dev),
                          dev, &local_err);
    } else {
        error_setg(&local_err, "device unplug for unsupported device"
                  " type: %s", object_get_typename(OBJECT(dev)));
    }

    if (local_err) {
        goto out;
    }

    pc_dimm_unplug(PC_DIMM(dev), MACHINE(ms));
    object_unparent(OBJECT(dev));

out:
    return;
}

static void core4_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);

    mc->desc = "CORE4 BOARD";
    mc->init = core4_init;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = MAX_CPUS_CORE4;
    mc->reset = sw64_board_reset;
    mc->possible_cpu_arch_ids = sw64_possible_cpu_arch_ids;
    mc->default_cpu_type = SW64_CPU_TYPE_NAME("core4");
    mc->default_ram_id = "ram";
    mc->get_hotplug_handler = sw64_get_hotplug_handler;
    hc->pre_plug = core4_machine_device_pre_plug_cb;
    hc->plug = core4_machine_device_plug_cb;
    hc->unplug_request = core4_machine_device_unplug_request_cb;
    hc->unplug = core4_machine_device_unplug_cb;
}

static const TypeInfo core4_machine_info = {
    .name = TYPE_CORE4_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(CORE4MachineState),
    .class_size = sizeof(CORE4MachineClass),
    .class_init = core4_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
    },
};

static void core4_machine_init(void)
{
    type_register_static(&core4_machine_info);
}

type_init(core4_machine_init)
