/*
 * Virtio MEM device
 *
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * Authors:
 *  David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "sysemu/numa.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "hw/virtio/virtio-mem.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "exec/ram_addr.h"
#include "migration/misc.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "config-devices.h"

/*
 * Use QEMU_VMALLOC_ALIGN, so no THP will have to be split when unplugging
 * memory (e.g., 2MB on x86_64).
 */
#define VIRTIO_MEM_MIN_BLOCK_SIZE QEMU_VMALLOC_ALIGN
/*
 * Size the usable region bigger than the requested size if possible. Esp.
 * Linux guests will only add (aligned) memory blocks in case they fully
 * fit into the usable region, but plug+online only a subset of the pages.
 * The memory block size corresponds mostly to the section size.
 *
 * This allows e.g., to add 20MB with a section size of 128MB on x86_64, and
 * a section size of 1GB on arm64 (as long as the start address is properly
 * aligned, similar to ordinary DIMMs).
 *
 * We can change this at any time and maybe even make it configurable if
 * necessary (as the section size can change). But it's more likely that the
 * section size will rather get smaller and not bigger over time.
 */
#if defined(TARGET_X86_64) || defined(TARGET_I386)
#define VIRTIO_MEM_USABLE_EXTENT (2 * (128 * MiB))
#else
#error VIRTIO_MEM_USABLE_EXTENT not defined
#endif

static bool virtio_mem_is_busy(void)
{
    /*
     * Postcopy cannot handle concurrent discards and we don't want to migrate
     * pages on-demand with stale content when plugging new blocks.
     */
    return migration_in_incoming_postcopy();
}

static bool virtio_mem_test_bitmap(VirtIOMEM *vmem, uint64_t start_gpa,
                                   uint64_t size, bool plugged)
{
    const unsigned long first_bit = (start_gpa - vmem->addr) / vmem->block_size;
    const unsigned long last_bit = first_bit + (size / vmem->block_size) - 1;
    unsigned long found_bit;

    /* We fake a shorter bitmap to avoid searching too far. */
    if (plugged) {
        found_bit = find_next_zero_bit(vmem->bitmap, last_bit + 1, first_bit);
    } else {
        found_bit = find_next_bit(vmem->bitmap, last_bit + 1, first_bit);
    }
    return found_bit > last_bit;
}

static void virtio_mem_set_bitmap(VirtIOMEM *vmem, uint64_t start_gpa,
                                  uint64_t size, bool plugged)
{
    const unsigned long bit = (start_gpa - vmem->addr) / vmem->block_size;
    const unsigned long nbits = size / vmem->block_size;

    if (plugged) {
        bitmap_set(vmem->bitmap, bit, nbits);
    } else {
        bitmap_clear(vmem->bitmap, bit, nbits);
    }
}

static void virtio_mem_send_response(VirtIOMEM *vmem, VirtQueueElement *elem,
                                     struct virtio_mem_resp *resp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vmem);
    VirtQueue *vq = vmem->vq;

    iov_from_buf(elem->in_sg, elem->in_num, 0, resp, sizeof(*resp));

    virtqueue_push(vq, elem, sizeof(*resp));
    virtio_notify(vdev, vq);
}

static void virtio_mem_send_response_simple(VirtIOMEM *vmem,
                                            VirtQueueElement *elem,
                                            uint16_t type)
{
    struct virtio_mem_resp resp = {
        .type = cpu_to_le16(type),
    };

    virtio_mem_send_response(vmem, elem, &resp);
}

static bool virtio_mem_valid_range(VirtIOMEM *vmem, uint64_t gpa, uint64_t size)
{
    if (!QEMU_IS_ALIGNED(gpa, vmem->block_size)) {
        return false;
    }
    if (gpa + size < gpa || !size) {
        return false;
    }
    if (gpa < vmem->addr || gpa >= vmem->addr + vmem->usable_region_size) {
        return false;
    }
    if (gpa + size > vmem->addr + vmem->usable_region_size) {
        return false;
    }
    return true;
}

static int virtio_mem_set_block_state(VirtIOMEM *vmem, uint64_t start_gpa,
                                      uint64_t size, bool plug)
{
    const uint64_t offset = start_gpa - vmem->addr;
    int ret;

    if (virtio_mem_is_busy()) {
        return -EBUSY;
    }

    if (!plug) {
        ret = ram_block_discard_range(vmem->memdev->mr.ram_block, offset, size);
        if (ret) {
            error_report("Unexpected error discarding RAM: %s",
                         strerror(-ret));
            return -EBUSY;
        }
    }
    virtio_mem_set_bitmap(vmem, start_gpa, size, plug);
    return 0;
}

static int virtio_mem_state_change_request(VirtIOMEM *vmem, uint64_t gpa,
                                           uint16_t nb_blocks, bool plug)
{
    const uint64_t size = nb_blocks * vmem->block_size;
    int ret;

    if (!virtio_mem_valid_range(vmem, gpa, size)) {
        return VIRTIO_MEM_RESP_ERROR;
    }

    if (plug && (vmem->size + size > vmem->requested_size)) {
        return VIRTIO_MEM_RESP_NACK;
    }

    /* test if really all blocks are in the opposite state */
    if (!virtio_mem_test_bitmap(vmem, gpa, size, !plug)) {
        return VIRTIO_MEM_RESP_ERROR;
    }

    ret = virtio_mem_set_block_state(vmem, gpa, size, plug);
    if (ret) {
        return VIRTIO_MEM_RESP_BUSY;
    }
    if (plug) {
        vmem->size += size;
    } else {
        vmem->size -= size;
    }
    return VIRTIO_MEM_RESP_ACK;
}

static void virtio_mem_plug_request(VirtIOMEM *vmem, VirtQueueElement *elem,
                                    struct virtio_mem_req *req)
{
    const uint64_t gpa = le64_to_cpu(req->u.plug.addr);
    const uint16_t nb_blocks = le16_to_cpu(req->u.plug.nb_blocks);
    uint16_t type;

    type = virtio_mem_state_change_request(vmem, gpa, nb_blocks, true);
    virtio_mem_send_response_simple(vmem, elem, type);
}

static void virtio_mem_unplug_request(VirtIOMEM *vmem, VirtQueueElement *elem,
                                      struct virtio_mem_req *req)
{
    const uint64_t gpa = le64_to_cpu(req->u.unplug.addr);
    const uint16_t nb_blocks = le16_to_cpu(req->u.unplug.nb_blocks);
    uint16_t type;

    type = virtio_mem_state_change_request(vmem, gpa, nb_blocks, false);
    virtio_mem_send_response_simple(vmem, elem, type);
}

static void virtio_mem_resize_usable_region(VirtIOMEM *vmem,
                                            uint64_t requested_size,
                                            bool can_shrink)
{
    uint64_t newsize = MIN(memory_region_size(&vmem->memdev->mr),
                           requested_size + VIRTIO_MEM_USABLE_EXTENT);

    if (!requested_size) {
        newsize = 0;
    }

    if (newsize < vmem->usable_region_size && !can_shrink) {
        return;
    }

    vmem->usable_region_size = newsize;
}

static int virtio_mem_unplug_all(VirtIOMEM *vmem)
{
    RAMBlock *rb = vmem->memdev->mr.ram_block;
    int ret;

    if (virtio_mem_is_busy()) {
        return -EBUSY;
    }

    ret = ram_block_discard_range(rb, 0, qemu_ram_get_used_length(rb));
    if (ret) {
        error_report("Unexpected error discarding RAM: %s", strerror(-ret));
        return -EBUSY;
    }
    bitmap_clear(vmem->bitmap, 0, vmem->bitmap_size);
    vmem->size = 0;

    virtio_mem_resize_usable_region(vmem, vmem->requested_size, true);
    return 0;
}

static void virtio_mem_unplug_all_request(VirtIOMEM *vmem,
                                          VirtQueueElement *elem)
{
    if (virtio_mem_unplug_all(vmem)) {
        virtio_mem_send_response_simple(vmem, elem, VIRTIO_MEM_RESP_BUSY);
    } else {
        virtio_mem_send_response_simple(vmem, elem, VIRTIO_MEM_RESP_ACK);
    }
}

static void virtio_mem_state_request(VirtIOMEM *vmem, VirtQueueElement *elem,
                                     struct virtio_mem_req *req)
{
    const uint16_t nb_blocks = le16_to_cpu(req->u.state.nb_blocks);
    const uint64_t gpa = le64_to_cpu(req->u.state.addr);
    const uint64_t size = nb_blocks * vmem->block_size;
    struct virtio_mem_resp resp = {
        .type = cpu_to_le16(VIRTIO_MEM_RESP_ACK),
    };

    if (!virtio_mem_valid_range(vmem, gpa, size)) {
        virtio_mem_send_response_simple(vmem, elem, VIRTIO_MEM_RESP_ERROR);
        return;
    }

    if (virtio_mem_test_bitmap(vmem, gpa, size, true)) {
        resp.u.state.state = cpu_to_le16(VIRTIO_MEM_STATE_PLUGGED);
    } else if (virtio_mem_test_bitmap(vmem, gpa, size, false)) {
        resp.u.state.state = cpu_to_le16(VIRTIO_MEM_STATE_UNPLUGGED);
    } else {
        resp.u.state.state = cpu_to_le16(VIRTIO_MEM_STATE_MIXED);
    }
    virtio_mem_send_response(vmem, elem, &resp);
}

static void virtio_mem_handle_request(VirtIODevice *vdev, VirtQueue *vq)
{
    const int len = sizeof(struct virtio_mem_req);
    VirtIOMEM *vmem = VIRTIO_MEM(vdev);
    VirtQueueElement *elem;
    struct virtio_mem_req req;
    uint16_t type;

    while (true) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &req, len) < len) {
            virtio_error(vdev, "virtio-mem protocol violation: invalid request"
                         " size: %d", len);
            g_free(elem);
            return;
        }

        if (iov_size(elem->in_sg, elem->in_num) <
            sizeof(struct virtio_mem_resp)) {
            virtio_error(vdev, "virtio-mem protocol violation: not enough space"
                         " for response: %zu",
                         iov_size(elem->in_sg, elem->in_num));
            g_free(elem);
            return;
        }

        type = le16_to_cpu(req.type);
        switch (type) {
        case VIRTIO_MEM_REQ_PLUG:
            virtio_mem_plug_request(vmem, elem, &req);
            break;
        case VIRTIO_MEM_REQ_UNPLUG:
            virtio_mem_unplug_request(vmem, elem, &req);
            break;
        case VIRTIO_MEM_REQ_UNPLUG_ALL:
            virtio_mem_unplug_all_request(vmem, elem);
            break;
        case VIRTIO_MEM_REQ_STATE:
            virtio_mem_state_request(vmem, elem, &req);
            break;
        default:
            virtio_error(vdev, "virtio-mem protocol violation: unknown request"
                         " type: %d", type);
            g_free(elem);
            return;
        }

        g_free(elem);
    }
}

static void virtio_mem_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOMEM *vmem = VIRTIO_MEM(vdev);
    struct virtio_mem_config *config = (void *) config_data;

    config->block_size = cpu_to_le64(vmem->block_size);
    config->node_id = cpu_to_le16(vmem->node);
    config->requested_size = cpu_to_le64(vmem->requested_size);
    config->plugged_size = cpu_to_le64(vmem->size);
    config->addr = cpu_to_le64(vmem->addr);
    config->region_size = cpu_to_le64(memory_region_size(&vmem->memdev->mr));
    config->usable_region_size = cpu_to_le64(vmem->usable_region_size);
}

static uint64_t virtio_mem_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    if (ms->numa_state) {
#if defined(CONFIG_ACPI)
        virtio_add_feature(&features, VIRTIO_MEM_F_ACPI_PXM);
#endif
    }
    return features;
}

static void virtio_mem_system_reset(void *opaque)
{
    VirtIOMEM *vmem = VIRTIO_MEM(opaque);

    /*
     * During usual resets, we will unplug all memory and shrink the usable
     * region size. This is, however, not possible in all scenarios. Then,
     * the guest has to deal with this manually (VIRTIO_MEM_REQ_UNPLUG_ALL).
     */
    virtio_mem_unplug_all(vmem);
}

static void virtio_mem_device_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    int nb_numa_nodes = ms->numa_state ? ms->numa_state->num_nodes : 0;
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOMEM *vmem = VIRTIO_MEM(dev);
    uint64_t page_size;
    RAMBlock *rb;
    int ret;

    if (!vmem->memdev) {
        error_setg(errp, "'%s' property is not set", VIRTIO_MEM_MEMDEV_PROP);
        return;
    } else if (host_memory_backend_is_mapped(vmem->memdev)) {
        char *path = object_get_canonical_path_component(OBJECT(vmem->memdev));

        error_setg(errp, "'%s' property specifies a busy memdev: %s",
                   VIRTIO_MEM_MEMDEV_PROP, path);
        g_free(path);
        return;
    } else if (!memory_region_is_ram(&vmem->memdev->mr) ||
        memory_region_is_rom(&vmem->memdev->mr) ||
        !vmem->memdev->mr.ram_block) {
        error_setg(errp, "'%s' property specifies an unsupported memdev",
                   VIRTIO_MEM_MEMDEV_PROP);
        return;
    }

    if ((nb_numa_nodes && vmem->node >= nb_numa_nodes) ||
        (!nb_numa_nodes && vmem->node)) {
        error_setg(errp, "'%s' property has value '%" PRIu32 "', which exceeds"
                   "the number of numa nodes: %d", VIRTIO_MEM_NODE_PROP,
                   vmem->node, nb_numa_nodes ? nb_numa_nodes : 1);
        return;
    }

    if (enable_mlock) {
        error_setg(errp, "Incompatible with mlock");
        return;
    }

    rb = vmem->memdev->mr.ram_block;
    page_size = qemu_ram_pagesize(rb);

    if (vmem->block_size < page_size) {
        error_setg(errp, "'%s' property has to be at least the page size (0x%"
                   PRIx64 ")", VIRTIO_MEM_BLOCK_SIZE_PROP, page_size);
        return;
    } else if (!QEMU_IS_ALIGNED(vmem->requested_size, vmem->block_size)) {
        error_setg(errp, "'%s' property has to be multiples of '%s' (0x%" PRIx64
                   ")", VIRTIO_MEM_REQUESTED_SIZE_PROP,
                   VIRTIO_MEM_BLOCK_SIZE_PROP, vmem->block_size);
        return;
    } else if (!QEMU_IS_ALIGNED(memory_region_size(&vmem->memdev->mr),
                                vmem->block_size)) {
        error_setg(errp, "'%s' property memdev size has to be multiples of"
                   "'%s' (0x%" PRIx64 ")", VIRTIO_MEM_MEMDEV_PROP,
                   VIRTIO_MEM_BLOCK_SIZE_PROP, vmem->block_size);
        return;
    }

    if (ram_block_discard_require(true)) {
        error_setg(errp, "Discarding RAM is disabled");
        return;
    }

    ret = ram_block_discard_range(rb, 0, qemu_ram_get_used_length(rb));
    if (ret) {
        error_setg_errno(errp, -ret, "Unexpected error discarding RAM");
        ram_block_discard_require(false);
        return;
    }

    virtio_mem_resize_usable_region(vmem, vmem->requested_size, true);

    vmem->bitmap_size = memory_region_size(&vmem->memdev->mr) /
                        vmem->block_size;
    vmem->bitmap = bitmap_new(vmem->bitmap_size);

    virtio_init(vdev, TYPE_VIRTIO_MEM, VIRTIO_ID_MEM,
                sizeof(struct virtio_mem_config));
    vmem->vq = virtio_add_queue(vdev, 128, virtio_mem_handle_request);

    host_memory_backend_set_mapped(vmem->memdev, true);
    vmstate_register_ram(&vmem->memdev->mr, DEVICE(vmem));
    qemu_register_reset(virtio_mem_system_reset, vmem);
}

static void virtio_mem_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOMEM *vmem = VIRTIO_MEM(dev);

    qemu_unregister_reset(virtio_mem_system_reset, vmem);
    vmstate_unregister_ram(&vmem->memdev->mr, DEVICE(vmem));
    host_memory_backend_set_mapped(vmem->memdev, false);
    virtio_del_queue(vdev, 0);
    virtio_cleanup(vdev);
    g_free(vmem->bitmap);
    ram_block_discard_require(false);
}

static int virtio_mem_restore_unplugged(VirtIOMEM *vmem)
{
    RAMBlock *rb = vmem->memdev->mr.ram_block;
    unsigned long first_zero_bit, last_zero_bit;
    uint64_t offset, length;
    int ret;

    /* Find consecutive unplugged blocks and discard the consecutive range. */
    first_zero_bit = find_first_zero_bit(vmem->bitmap, vmem->bitmap_size);
    while (first_zero_bit < vmem->bitmap_size) {
        offset = first_zero_bit * vmem->block_size;
        last_zero_bit = find_next_bit(vmem->bitmap, vmem->bitmap_size,
                                      first_zero_bit + 1) - 1;
        length = (last_zero_bit - first_zero_bit + 1) * vmem->block_size;

        ret = ram_block_discard_range(rb, offset, length);
        if (ret) {
            error_report("Unexpected error discarding RAM: %s",
                         strerror(-ret));
            return -EINVAL;
        }
        first_zero_bit = find_next_zero_bit(vmem->bitmap, vmem->bitmap_size,
                                            last_zero_bit + 2);
    }
    return 0;
}

static int virtio_mem_post_load(void *opaque, int version_id)
{
    if (migration_in_incoming_postcopy()) {
        return 0;
    }

    return virtio_mem_restore_unplugged(VIRTIO_MEM(opaque));
}

static const VMStateDescription vmstate_virtio_mem_device = {
    .name = "virtio-mem-device",
    .minimum_version_id = 1,
    .version_id = 1,
    .post_load = virtio_mem_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(usable_region_size, VirtIOMEM),
        VMSTATE_UINT64(size, VirtIOMEM),
        VMSTATE_UINT64(requested_size, VirtIOMEM),
        VMSTATE_BITMAP(bitmap, VirtIOMEM, 0, bitmap_size),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_virtio_mem = {
    .name = "virtio-mem",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static void virtio_mem_fill_device_info(const VirtIOMEM *vmem,
                                        VirtioMEMDeviceInfo *vi)
{
    vi->memaddr = vmem->addr;
    vi->node = vmem->node;
    vi->requested_size = vmem->requested_size;
    vi->size = vmem->size;
    vi->max_size = memory_region_size(&vmem->memdev->mr);
    vi->block_size = vmem->block_size;
    vi->memdev = object_get_canonical_path(OBJECT(vmem->memdev));
}

static MemoryRegion *virtio_mem_get_memory_region(VirtIOMEM *vmem, Error **errp)
{
    if (!vmem->memdev) {
        error_setg(errp, "'%s' property must be set", VIRTIO_MEM_MEMDEV_PROP);
        return NULL;
    }

    return &vmem->memdev->mr;
}

static void virtio_mem_get_size(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    const VirtIOMEM *vmem = VIRTIO_MEM(obj);
    uint64_t value = vmem->size;

    visit_type_size(v, name, &value, errp);
}

static void virtio_mem_get_requested_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    const VirtIOMEM *vmem = VIRTIO_MEM(obj);
    uint64_t value = vmem->requested_size;

    visit_type_size(v, name, &value, errp);
}

static void virtio_mem_set_requested_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    VirtIOMEM *vmem = VIRTIO_MEM(obj);
    Error *err = NULL;
    uint64_t value;

    visit_type_size(v, name, &value, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    /*
     * The block size and memory backend are not fixed until the device was
     * realized. realize() will verify these properties then.
     */
    if (DEVICE(obj)->realized) {
        if (!QEMU_IS_ALIGNED(value, vmem->block_size)) {
            error_setg(errp, "'%s' has to be multiples of '%s' (0x%" PRIx64
                       ")", name, VIRTIO_MEM_BLOCK_SIZE_PROP,
                       vmem->block_size);
            return;
        } else if (value > memory_region_size(&vmem->memdev->mr)) {
            error_setg(errp, "'%s' cannot exceed the memory backend size"
                       "(0x%" PRIx64 ")", name,
                       memory_region_size(&vmem->memdev->mr));
            return;
        }

        if (value != vmem->requested_size) {
            virtio_mem_resize_usable_region(vmem, value, false);
            vmem->requested_size = value;
        }
        /*
         * Trigger a config update so the guest gets notified. We trigger
         * even if the size didn't change (especially helpful for debugging).
         */
        virtio_notify_config(VIRTIO_DEVICE(vmem));
    } else {
        vmem->requested_size = value;
    }
}

static void virtio_mem_get_block_size(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    const VirtIOMEM *vmem = VIRTIO_MEM(obj);
    uint64_t value = vmem->block_size;

    visit_type_size(v, name, &value, errp);
}

static void virtio_mem_set_block_size(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    VirtIOMEM *vmem = VIRTIO_MEM(obj);
    Error *err = NULL;
    uint64_t value;

    if (DEVICE(obj)->realized) {
        error_setg(errp, "'%s' cannot be changed", name);
        return;
    }

    visit_type_size(v, name, &value, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (value < VIRTIO_MEM_MIN_BLOCK_SIZE) {
        error_setg(errp, "'%s' property has to be at least 0x%" PRIx32, name,
                   VIRTIO_MEM_MIN_BLOCK_SIZE);
        return;
    } else if (!is_power_of_2(value)) {
        error_setg(errp, "'%s' property has to be a power of two", name);
        return;
    }
    vmem->block_size = value;
}

static void virtio_mem_instance_init(Object *obj)
{
    VirtIOMEM *vmem = VIRTIO_MEM(obj);

    vmem->block_size = VIRTIO_MEM_MIN_BLOCK_SIZE;

    object_property_add(obj, VIRTIO_MEM_SIZE_PROP, "size", virtio_mem_get_size,
                        NULL, NULL, NULL);
    object_property_add(obj, VIRTIO_MEM_REQUESTED_SIZE_PROP, "size",
                        virtio_mem_get_requested_size,
                        virtio_mem_set_requested_size, NULL, NULL);
    object_property_add(obj, VIRTIO_MEM_BLOCK_SIZE_PROP, "size",
                        virtio_mem_get_block_size, virtio_mem_set_block_size,
                        NULL, NULL);
}

static Property virtio_mem_properties[] = {
    DEFINE_PROP_UINT64(VIRTIO_MEM_ADDR_PROP, VirtIOMEM, addr, 0),
    DEFINE_PROP_UINT32(VIRTIO_MEM_NODE_PROP, VirtIOMEM, node, 0),
    DEFINE_PROP_LINK(VIRTIO_MEM_MEMDEV_PROP, VirtIOMEM, memdev,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_mem_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOMEMClass *vmc = VIRTIO_MEM_CLASS(klass);

    device_class_set_props(dc, virtio_mem_properties);
    dc->vmsd = &vmstate_virtio_mem;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_mem_device_realize;
    vdc->unrealize = virtio_mem_device_unrealize;
    vdc->get_config = virtio_mem_get_config;
    vdc->get_features = virtio_mem_get_features;
    vdc->vmsd = &vmstate_virtio_mem_device;

    vmc->fill_device_info = virtio_mem_fill_device_info;
    vmc->get_memory_region = virtio_mem_get_memory_region;
}

static const TypeInfo virtio_mem_info = {
    .name = TYPE_VIRTIO_MEM,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOMEM),
    .instance_init = virtio_mem_instance_init,
    .class_init = virtio_mem_class_init,
    .class_size = sizeof(VirtIOMEMClass),
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_mem_info);
}

type_init(virtio_register_types)
