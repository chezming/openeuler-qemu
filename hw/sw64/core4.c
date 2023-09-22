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
#include "hw/boards.h"
#include "sysemu/numa.h"

#define MAX_CPUS 64

static uint64_t cpu_sw64_virt_to_phys(void *opaque, uint64_t addr)
{
    return addr &= ~0xfff0000000000000;
}

static const CPUArchIdList *sw64_possible_cpu_arch_ids(MachineState *ms)
{
    int i;
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        /*
         * make sure that max_cpus hasn't changed since the first use, i.e.
         * -smp hasn't been parsed after it
        */
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (i = 0; i < ms->possible_cpus->len; i++) {
        ms->possible_cpus->cpus[i].type = ms->cpu_type;
        ms->possible_cpus->cpus[i].vcpus_count = 1;
        ms->possible_cpus->cpus[i].arch_id = i;
        ms->possible_cpus->cpus[i].props.has_thread_id = true;
        ms->possible_cpus->cpus[i].props.has_core_id = true;
        ms->possible_cpus->cpus[i].props.core_id = i;
    }

    return ms->possible_cpus;
}

#ifndef CONFIG_KVM
static void core4_cpu_reset(void *opaque)
{
    SW64CPU *cpu = opaque;

    cpu_reset(CPU(cpu));
}
#endif

static void core4_init(MachineState *machine)
{
    ram_addr_t ram_size = machine->ram_size;
    ram_addr_t buf;
    long size;
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    char *hmcode_filename;
    char *uefi_filename;
    uint64_t hmcode_entry, hmcode_low, hmcode_high;
    uint64_t kernel_entry, kernel_low, kernel_high;
    BOOT_PARAMS *core4_boot_params = g_new0(BOOT_PARAMS, 1);
    uint64_t param_offset;
    core4_board_init(machine);
    if (kvm_enabled())
        buf = ram_size;
    else
        buf = ram_size | (1UL << 63);

    rom_add_blob_fixed("ram_size", (char *)&buf, 0x8, 0x2040);

    param_offset = 0x90B000UL;
    core4_boot_params->cmdline = param_offset | 0xfff0000000000000UL;
    hmcode_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, kvm_enabled() ? "core4-reset":"core4-hmcode");
    if (hmcode_filename == NULL) {
        if (kvm_enabled())
	    error_report("no core4-reset provided");
        else
	    error_report("no core4-hmcode provided");
        exit(1);
    }
    size = load_elf(hmcode_filename, NULL, cpu_sw64_virt_to_phys, NULL,
                    &hmcode_entry, &hmcode_low, &hmcode_high, NULL, 0, EM_SW64, 0, 0);
    if (size < 0) {
        if (kvm_enabled())
	    error_report("could not load core4-reset: '%s'", hmcode_filename);
        else
	    error_report("could not load core4-hmcode: '%s'", hmcode_filename);
        exit(1);
    }
    g_free(hmcode_filename);
    if (!kernel_filename) {
        uefi_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, "core4-bios-virtio.bin");
	if (uefi_filename == NULL) {
	    error_report("no virtual bios provided");
	    exit(1);
	}
	size = load_image_targphys(uefi_filename, 0x2f00000UL, -1);
	if (size < 0) {
	    error_report("could not load virtual bios: '%s'", uefi_filename);
	    exit(1);
	}
        g_free(uefi_filename);
    } else {
	    /* Load a kernel.  */
        size = load_elf(kernel_filename, NULL, cpu_sw64_virt_to_phys, NULL,
                        &kernel_entry, &kernel_low, &kernel_high, NULL, 0, EM_SW64, 0,
                        0);

        if (size < 0) {
            error_report("could not load kernel '%s'", kernel_filename);
            exit(1);
        }
        if (kernel_cmdline) {
            pstrcpy_targphys("cmdline", param_offset, 0x400, kernel_cmdline);
        }
    }

    if (initrd_filename) {
        long initrd_base, initrd_size;

        initrd_size = get_image_size(initrd_filename);
        if (initrd_size < 0) {
            error_report("could not load initial ram disk '%s'",
                         initrd_filename);
            exit(1);
        }
        // Put the initrd image as high in memory as possible.
        initrd_base = 0x3000000UL;
        load_image_targphys(initrd_filename, initrd_base, initrd_size);
        core4_boot_params->initrd_start = initrd_base | 0xfff0000000000000UL;
        core4_boot_params->initrd_size = initrd_size;
        rom_add_blob_fixed("core4_boot_params", (core4_boot_params), 0x48, 0x90A100);
    }

#ifndef CONFIG_KVM
    CPUState *cpu;
    SW64CPU *sw64_cpu;
    CPU_FOREACH(cpu) {
        sw64_cpu = SW64_CPU(cpu);
        sw64_cpu->env.pc = hmcode_entry;
        sw64_cpu->env.hm_entry = hmcode_entry;
        sw64_cpu->env.csr[CID] = sw64_cpu->cid;
        qemu_register_reset(core4_cpu_reset, sw64_cpu);
    }
#endif
}

static void board_reset(MachineState *state)
{
    qemu_devices_reset();
}

static void core4_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "CORE4 BOARD";
    mc->init = core4_init;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = MAX_CPUS;
    mc->reset = board_reset;
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
