/*
 * QEMU host SGX EPC memory backend
 *
 * Copyright (C) 2019 Intel Corporation
 *
 * Authors:
 *   Sean Christopherson <sean.j.christopherson@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include <sys/ioctl.h>

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qom/object_interfaces.h"
#include "qapi/error.h"
#include "sysemu/hostmem.h"
#include "hw/i386/hostmem-epc.h"
#include "migration/cpr-state.h"
#include "migration/misc.h"

static void
sgx_epc_backend_memory_alloc(HostMemoryBackend *backend, Error **errp)
{
    uint32_t ram_flags;
    g_autofree char *name = object_get_canonical_path(OBJECT(backend));
    int fd = cpr_find_fd(name, 0);

    if (!backend->size) {
        error_setg(errp, "can't create backend with size 0");
        return;
    }

    if (fd < 0) {
        fd = qemu_open_old("/dev/sgx_vepc", O_RDWR);
        if (fd < 0) {
            error_setg_errno(errp, errno,
                             "failed to open /dev/sgx_vepc to alloc SGX EPC");
            return;
        }
        cpr_save_fd(name, 0, fd);
    }

    ram_flags = (backend->share ? RAM_SHARED : 0) | RAM_PROTECTED;
    if (migrate_mode() == MIG_MODE_CPR_EXEC)
        ram_flags |= RAM_MAP_FIXED;
    memory_region_init_ram_from_fd(&backend->mr, OBJECT(backend),
                                   name, backend->size, ram_flags,
                                   fd, 0, errp);
}

static void sgx_epc_backend_instance_init(Object *obj)
{
    HostMemoryBackend *m = MEMORY_BACKEND(obj);

    m->share = true;
    m->merge = false;
    m->dump = false;
}

static void sgx_epc_backend_class_init(ObjectClass *oc, void *data)
{
    HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);

    bc->alloc = sgx_epc_backend_memory_alloc;
}

static const TypeInfo sgx_epc_backed_info = {
    .name = TYPE_MEMORY_BACKEND_EPC,
    .parent = TYPE_MEMORY_BACKEND,
    .instance_init = sgx_epc_backend_instance_init,
    .class_init = sgx_epc_backend_class_init,
    .instance_size = sizeof(HostMemoryBackendEpc),
};

static void register_types(void)
{
    int fd = qemu_open_old("/dev/sgx_vepc", O_RDWR);
    if (fd >= 0) {
        close(fd);

        type_register_static(&sgx_epc_backed_info);
    }
}

type_init(register_types);
