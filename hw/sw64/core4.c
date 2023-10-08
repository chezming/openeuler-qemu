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

static void core4_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "CORE4 BOARD";
    mc->init = core4_init;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = MAX_CPUS_CORE4;
    mc->reset = sw64_board_reset;
    mc->possible_cpu_arch_ids = sw64_possible_cpu_arch_ids;
    mc->default_cpu_type = SW64_CPU_TYPE_NAME("core4");
    mc->default_ram_id = "ram";
}

static const TypeInfo core4_machine_info = {
    .name          = TYPE_CORE4_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = core4_machine_class_init,
    .instance_size = sizeof(CORE4MachineState),
};

static void core4_machine_init(void)
{
    type_register_static(&core4_machine_info);
}
type_init(core4_machine_init);
