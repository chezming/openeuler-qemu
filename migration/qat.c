#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/thread.h"
#include "qemu/log.h"
#include "exec/ram_addr.h"
#include "migration.h"
#include "qemu-file.h"
#include <cpa.h>
#include <cpa_dc.h>
#include <icp_sal_poll.h>
#include <icp_sal_user.h>
#include <qae_mem.h>
#include <qae_mem_utils.h>
#include "qat.h"
#include "qemu-common.h"
#define QAT_COMP_REQ_BUF_SIZE (RAM_SAVE_MULTI_PAGE_NUM << TARGET_PAGE_BITS)
#define QAT_DECOMP_REQ_BUF_SIZE (RAM_SAVE_MAX_PAGE_NUM << TARGET_PAGE_BITS)

#define MAX_PROCESS_NUM 4
static int g_instReqCacheNum = 32;

typedef struct _qat_inst_t {
    uint16_t id; // debug purpose
    int fd;
    uint32_t node_affinity;
    uint32_t req_cache_num;
    CpaInstanceHandle inst_handle;
    CpaDcSessionHandle sess_handle;
    QLIST_HEAD(, qat_req_t) req_cache_list;
    int src_buf_num;
    int src_buf_size;
    int dst_buf_num;
    int dst_buf_size;
    int queue_depth;
} qat_inst_t;
typedef struct qat_req_t {
     /*
      * For decompression, stores the checkum passed from the compression side.
      * For compresssion, not used.
      */
     uint32_t checksum;
     uint64_t id; // debug purpose
     qat_inst_t *inst;
     RAMBlock *block;
     ram_addr_t offset;
     MultiPageAddr mpa;
     CpaBufferList *src_buf_list;
     CpaBufferList *dst_buf_list;
     CpaDcRqResults result;
     uint32_t expect;
     QLIST_ENTRY(qat_req_t) node;
} qat_req_t;
typedef struct _qat_dev_t {
    bool svm_enabled;
    bool zero_copy;
    qat_setup_type_t type;
    QEMUFile *f;
    uint16_t inst_num;
    CpaInstanceHandle *inst_handles;
    uint32_t meta_buf_size;
    qat_inst_t *insts;
    QemuThread epoll_thread;
    int efd;
    /* Fill instances in round robin */
    int rr_inst_id;
    uint64_t requests;
    uint64_t responses;
    uint64_t overflow;
    QLIST_HEAD(, qat_req_t) req_post_list;
    QemuSpin lock;
    uint32_t req_post_num;
    bool flush_stage;
    int state; // -1 indicates error state
    QemuMutex mutex;
    QemuCond cond;
} qat_dev_t;
static qat_dev_t *qat_dev = NULL;
static bool epoll_thread_running = false;

typedef void* (*qat_mem_alloc_t)(Cpa32U size, Cpa32U node, Cpa32U alignment);
typedef void (*qat_mem_free_t)(void **p);
typedef uint64_t (*qat_addr_translate_t)(void *virt);

static qat_mem_alloc_t qat_mem_alloc = NULL;
static qat_mem_free_t qat_mem_free = NULL;
static qat_addr_translate_t qat_addr_translate = NULL;
int qat_send_req(qat_req_t *req);
static void *qat_mem_alloc_phy(Cpa32U size, Cpa32U node, Cpa32U alignment)
{
    return qaeMemAllocNUMA(size, node, alignment);
}

static void qat_mem_free_phy(void **p)
{
    if (NULL != *p) {
        qaeMemFreeNUMA(p);
        *p = NULL;
    }
}
static void* qat_mem_alloc_virt(Cpa32U size, Cpa32U node, Cpa32U alignment)
{
    return malloc(size);
}

static void qat_mem_free_virt(void **p)
{
    if (p != NULL && NULL != *p) {
        free((void*)*p);
        *p = NULL;
    }
}
static uint64_t qat_virt_to_phy_svm0(void *vaddr)
{
    uint64_t paddr = qaeVirtToPhysNUMA(vaddr);
    if (!paddr)
        error_report("%s: meta_buf fail to get pa for vaddr=%p", __func__, vaddr);
    return paddr;
}
static uint64_t qat_virt_to_phy_svm1(void *vaddr)
{
    return (uint64_t)vaddr;
}
static CpaBufferList *qat_buf_list_alloc(int nodeid, int buf_num, int buf_size)
{
    CpaBufferList *buf_list = NULL;
    Cpa8U *meta_buf = NULL;
    CpaFlatBuffer *flat_buf = NULL;
    Cpa32U buf_list_size;

    buf_list_size = sizeof(CpaBufferList) + sizeof(CpaFlatBuffer) * buf_num;

    buf_list = g_malloc0(buf_list_size);
    if (unlikely(buf_list == NULL)) {
        error_report("%s: unable to alloc buf list", __func__);
        return NULL;
    }

    meta_buf = qat_mem_alloc(qat_dev->meta_buf_size, nodeid, 64);
    if (unlikely(meta_buf == NULL)) {
        error_report("%s: unable to alloc src_meta_buf", __func__);
        goto err_free_buf_list;
    }
    flat_buf = (CpaFlatBuffer*)(buf_list + 1);
    if (buf_size) {
        for (int i = 0; i < buf_num; i++) {
            flat_buf[i].pData = qat_mem_alloc(buf_size, nodeid, 64);
            if (!flat_buf[i].pData) {
                error_report("%s: unable to alloc src buf", __func__);
                goto err_free_meta_buf;
            }
            flat_buf[i].dataLenInBytes = buf_size;
        }
    }

    buf_list->pPrivateMetaData = meta_buf;
    buf_list->pBuffers = flat_buf;
    buf_list->numBuffers = buf_num;

    return buf_list;
err_free_buf_list:
    g_free(buf_list);
err_free_meta_buf:
    qat_mem_free((void **)&meta_buf);
    return NULL;
}

static void qat_buf_list_set_bufs_from_mpa(CpaBufferList *buf_list,
                                           unsigned long addr_base,
                                           MultiPageAddr *mpa)
{
    uint64_t start, offset, addr, pages;
    CpaFlatBuffer *flat_buf;
    uint8_t *p;
    flat_buf = (CpaFlatBuffer *)(buf_list + 1);
    flat_buf->dataLenInBytes = 0;
    p = flat_buf->pData;
    for (int i = 0; i < mpa->last_idx; i++) {
        start = multi_page_addr_get_one(mpa, i);
        pages = start & (~TARGET_PAGE_MASK);
        start >>= TARGET_PAGE_BITS;
        for (int j = 0; j < pages; j++) {
            offset = (start + j) << TARGET_PAGE_BITS;
            addr = addr_base + offset;
            if (qat_dev->zero_copy) {
                int b = ((int*)(addr))[0]; b--; // avoid page fault
                flat_buf->pData = (uint8_t*)(addr);
                flat_buf->dataLenInBytes = TARGET_PAGE_SIZE;
                flat_buf++;
            } else {
                if (qat_dev->type == QAT_SETUP_COMPRESS) {
                    // only compression needs this copy
                    memcpy(p, (uint8_t*)(addr), TARGET_PAGE_SIZE);
                    p += TARGET_PAGE_SIZE;
                    flat_buf->dataLenInBytes += TARGET_PAGE_SIZE;
                }
            }
        }
    }

    buf_list->numBuffers = qat_dev->zero_copy ? mpa->pages : 1;
}

static void qat_buf_list_free(CpaBufferList *buf_list, bool buf_allocated)
{
    CpaFlatBuffer *flat_buf;

    if (unlikely(buf_list == NULL))
        return;

    if (buf_list->pPrivateMetaData)
        qat_mem_free((void**)&buf_list->pPrivateMetaData);

    flat_buf = buf_list->pBuffers;
    if (unlikely(flat_buf == NULL))
        return;

    if (buf_allocated) {
        for (int i = 0; i < buf_list->numBuffers; i++) {
            if (!flat_buf[i].pData)
                continue;
            qat_mem_free((void**)&flat_buf[i].pData);
        }
    }

    g_free(buf_list);
}

static void qat_inst_req_free(qat_req_t *req)
{
    qat_inst_t *inst = req->inst;
    if (inst->req_cache_num < g_instReqCacheNum) {
        QLIST_INSERT_HEAD(&inst->req_cache_list, req, node);
        inst->req_cache_num++;
    } else {
        qat_buf_list_free(req->src_buf_list, inst->src_buf_size);
        qat_buf_list_free(req->dst_buf_list, inst->dst_buf_size);
        g_free(req);
    }
}
static void qat_inst_req_free_lock(qat_req_t *req)
{
    qemu_spin_lock(&qat_dev->lock);
    qat_inst_req_free(req);
    qemu_spin_unlock(&qat_dev->lock);
}
static qat_req_t *qat_inst_req_alloc_cache(qat_inst_t *inst)
{
    qat_req_t *req = NULL;
    if (!inst->req_cache_num)
        return NULL;

    req = QLIST_FIRST(&inst->req_cache_list);
    QLIST_REMOVE(req, node);
    inst->req_cache_num--;

    return req;
}

static qat_req_t *qat_inst_req_alloc_slow(qat_inst_t *inst)
{
    qat_req_t *req;
    CpaBufferList *src_buf_list, *dst_buf_list;

    req = g_malloc0(sizeof(qat_req_t));
    src_buf_list = qat_buf_list_alloc(inst->node_affinity,
        inst->src_buf_num, inst->src_buf_size);
    if (unlikely(src_buf_list == NULL))
        goto err_src;

    dst_buf_list = qat_buf_list_alloc(inst->node_affinity,
        inst->dst_buf_num, inst->dst_buf_size);
    if (unlikely(dst_buf_list == NULL))
        goto err_dst;

    req->src_buf_list = src_buf_list;
    req->dst_buf_list = dst_buf_list;
    req->inst = inst;

    return req;
err_dst:
    qat_buf_list_free(src_buf_list, inst->src_buf_size);
err_src:
    g_free(req);
    error_report("%s: fail to alloc a qat req", __func__);
    return NULL;
}

static qat_req_t *qat_inst_req_alloc(qat_inst_t *inst)
{
    qat_req_t *req = qat_inst_req_alloc_cache(inst);

    if (unlikely(req == NULL)) {
        req = qat_inst_req_alloc_slow(inst);
    }

    return req;
}

static qat_req_t *qat_inst_req_alloc_lock(qat_inst_t *inst)
{
    qemu_spin_lock(&qat_dev->lock);
    qat_req_t *req = qat_inst_req_alloc(inst);
    qemu_spin_unlock(&qat_dev->lock);
    return req;
}

static void compress_callback(void *cb_ctx, CpaStatus status)
{
    qat_req_t *req = (qat_req_t*)cb_ctx;
    if (unlikely(req == NULL)) {
        error_report("%s: Compression with NULL request ptr", __func__);
        return;
    }
    req->inst->queue_depth--;
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        qat_inst_req_free(req);
        qat_dev->responses++;
        error_report("%s: Compress error: %x, ram addr: %lx", __func__, status, req->offset);
        qat_dev->state = -1;
        return;
    }

    // put the req into the send list
    qemu_spin_lock(&qat_dev->lock);
    if (!qat_dev->flush_stage) {
        QLIST_INSERT_HEAD(&qat_dev->req_post_list, req, node);
        qat_dev->req_post_num++;
    } else {
        qat_req_t *prev_req;
        while (qat_dev->req_post_num > 0) {
            prev_req = QLIST_FIRST(&qat_dev->req_post_list);
            QLIST_REMOVE(prev_req, node);
            qat_dev->req_post_num--;
            qat_send_req(prev_req);
        }
        qat_send_req(req);
        if (qat_dev->requests == qat_dev->responses) {
            qemu_cond_signal(&qat_dev->cond);
        }
    }

    qemu_spin_unlock(&qat_dev->lock);
}

static void decompress_copy_to_guest_memory(qat_req_t *req)
{
    MultiPageAddr *mpa = &req->mpa;
    uint8_t *p = req->dst_buf_list->pBuffers[0].pData;
    unsigned long start, pages;
    uint8_t *dst_buf;

    for (int i = 0; i < mpa->last_idx; i++) {
        start = multi_page_addr_get_one(&req->mpa, i);
        pages = start & (~TARGET_PAGE_MASK);
        start &= TARGET_PAGE_MASK;
        for (int j = 0; j < pages; j++) {
            dst_buf = req->block->host + start + (j << TARGET_PAGE_BITS);
            memcpy(dst_buf, p, TARGET_PAGE_SIZE);
            p += TARGET_PAGE_SIZE;
        }
    }
}

static void decompress_callback(void *cb_ctx, CpaStatus status)
{
    qat_req_t *req = (qat_req_t*)cb_ctx;
    CpaDcRqResults *result;

    if (unlikely(req == NULL)) {
        error_report("%s: Compression with NULL request ptr", __func__);
        return;
    }
    req->inst->queue_depth--;
    result = &req->result;

    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: Decompress failed %d, ram addr=%lx, req->id=%ld",
                  __func__, status, req->offset, req->id);
        qat_dev->state = -1;
        goto decompress_err;
    } else if (unlikely(result->checksum != req->checksum)) {
        error_report("%s: error, checksum unmatch", __func__);
        qat_dev->state = -1;
        goto decompress_err;
    } else if (unlikely((result->status != CPA_DC_OK) &&
        (result->status == CPA_DC_OVERFLOW))) {
        error_report("%s: Decompress error: %d, consumed: %d, produced: %d",
                __func__, result->status, result->consumed, result->produced);
        qat_dev->state = -1;
        goto decompress_err;
    } else if (unlikely(result->produced != req->expect)) {
        error_report("%s: unmatched, consumed=%d, produced=%d, expect=%d",
                __func__, result->consumed, result->produced, req->expect);
        qat_dev->state = -1;
        goto decompress_err;
    }

    if (!qat_dev->zero_copy) {
        decompress_copy_to_guest_memory(req);
    }

decompress_err:
    qat_inst_req_free_lock(req);
    qat_dev->responses++;
    if (qat_dev->flush_stage && (qat_dev->requests == qat_dev->responses)) {
        qemu_cond_signal(&qat_dev->cond);
    }
}

static int qat_inst_session_setup(qat_inst_t *inst, qat_setup_type_t type)
{
    CpaInstanceHandle inst_handle = inst->inst_handle;
    CpaDcInstanceCapabilities cap = {0};
    CpaDcSessionHandle sess_handle = NULL;
    Cpa32U session_size = 0, ctx_size = 0;
    CpaDcSessionSetupData sd = { 0 };
    CpaDcCallbackFn session_callback;
    CpaStatus status;

    sd.compLevel = migrate_compress_level();
    sd.compType = CPA_DC_DEFLATE;
    sd.huffType = CPA_DC_HT_FULL_DYNAMIC;
    sd.autoSelectBestHuffmanTree = CPA_DC_ASB_DISABLED;
    sd.sessState = CPA_DC_STATELESS;
#if (CPA_DC_API_VERSION_NUM_MAJOR == 1 && CPA_DC_API_VERSION_NUM_MINOR < 6)
    sd.deflateWindowSize = 7;
#endif
    sd.checksum = CPA_DC_CRC32;
    if (type == QAT_SETUP_COMPRESS) {
        sd.sessDirection = CPA_DC_DIR_COMPRESS;
        session_callback = compress_callback;
    } else {
        sd.sessDirection = CPA_DC_DIR_DECOMPRESS;
        session_callback = decompress_callback;
    }

    status = cpaDcQueryCapabilities(inst_handle, &cap);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to get cap", __func__);
        return -1;
    }
    if (unlikely(!cap.checksumCRC32 || !cap.compressAndVerify)) {
        error_report("%s: checksum isn't supported", __func__);
        return -1;
    }

    status = cpaDcGetSessionSize(inst_handle, &sd, &session_size, &ctx_size);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to get session size", __func__);
        return -1;
    }

    sess_handle = qat_mem_alloc(session_size + ctx_size,
                                inst->node_affinity, 64);
    if (unlikely(sess_handle == NULL)) {
        error_report("%s: fail to alloc session handle", __func__);
        return -1;
    }

    status = cpaDcInitSession(inst_handle, sess_handle, &sd,
                              NULL, session_callback);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to init session", __func__);
        goto err_free_sess_handle;
    }
    inst->sess_handle = sess_handle;

    return 0;
err_free_sess_handle:
    qat_mem_free((void **)&sess_handle);
    return -1;
}

static int qat_inst_add_to_epoll(qat_inst_t *inst)
{
    CpaStatus status;
    struct epoll_event event;
    int fd, ret;

    status = icp_sal_DcGetFileDescriptor(inst->inst_handle, &fd);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to get instance poll fd", __func__);
        return -1;
    }

    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    ret = epoll_ctl(qat_dev->efd, EPOLL_CTL_ADD, fd, &event);
    if (unlikely(ret < 0)) {
        error_report("%s: fail to add to epoll list, ret=%d", __func__, ret);
        return -1;
    }
    inst->fd = fd;

    return 0;
}

static inline int qat_poll_insts(void)
{
    for (int i = 0; i < qat_dev->inst_num; i++) {
        qat_inst_t *inst = &qat_dev->insts[i];
        CpaStatus status = icp_sal_DcPollInstance(inst->inst_handle, 0);
        if (unlikely((status != CPA_STATUS_SUCCESS) &&
            (status != CPA_STATUS_RETRY))) {
            error_report("%s: fail to poll instance, i=%d, status=%d", __func__, i, status);
            qat_dev->state = -1;
            continue;
        }
    }

    return 0;
}

void *qat_epoll_thread_run(void *arg)
{
    int maxevents = (int)(qat_dev->inst_num);
    struct epoll_event *events =
                       g_malloc0(sizeof(struct epoll_event) * maxevents);
    while (epoll_thread_running) {
        epoll_wait(qat_dev->efd, events, maxevents, 100);
        qat_poll_insts();
    }

    g_free(events);
    return NULL;
}

static inline qat_inst_t *qat_select_inst_rr(void)
{
    qat_dev->rr_inst_id = (qat_dev->rr_inst_id + 1) % qat_dev->inst_num;
    return &qat_dev->insts[qat_dev->rr_inst_id];
}

static qat_req_t *qat_get_compress_req(qat_inst_t *inst,
                                    RAMBlock *block,
                                    MultiPageAddr *mpa)
{
    qat_req_t *req = qat_inst_req_alloc(inst);
    if (unlikely(req == NULL))
        return NULL;

    req->block = block;
    req->offset = multi_page_addr_get_one(mpa, 0);

    qat_buf_list_set_bufs_from_mpa(req->src_buf_list,
                                   (uint64_t)block->host, mpa);
    if (qat_dev->zero_copy) {
        // avoid page fault
        uint8_t *p = req->dst_buf_list->pBuffers[0].pData;
        for (int i = 0; i < RAM_SAVE_MULTI_PAGE_NUM; i++) {
            uint8_t a = p[0]; a--;
            p += TARGET_PAGE_SIZE;
        }
    }
    memcpy(&req->mpa, mpa, sizeof(MultiPageAddr));
    return req;
}
static qat_req_t *qat_get_decompress_req(qat_inst_t *inst,
                                      QEMUFile *f,
                                      RAMBlock *block,
                                      int src_bytes,
                                      MultiPageAddr *mpa)
{
    qat_req_t *req = qat_inst_req_alloc_lock(inst);
    if (unlikely(req == NULL))
        return NULL;
    
    req->block = block;
    req->offset = multi_page_addr_get_one(mpa, 0);
    req->expect = mpa->pages * TARGET_PAGE_SIZE;
    if (qat_dev->zero_copy) {
        qat_buf_list_set_bufs_from_mpa(req->dst_buf_list,
                                   (uint64_t)block->host, mpa);
    } else {
        memcpy(&req->mpa, mpa, sizeof(MultiPageAddr));
    }
 
    size_t size = qemu_get_buffer(f, req->src_buf_list->pBuffers[0].pData, src_bytes);
    if (unlikely(size != src_bytes)) {
        error_report("%s: not read enough data, %d, %lu", __func__, src_bytes, size);
        return NULL;
    }
    req->src_buf_list->pBuffers[0].dataLenInBytes = src_bytes;

    return req;
}
int qat_send_req(qat_req_t *req)
{
    CpaBufferList *buf_list;
    CpaDcRqResults *result;
    buf_list = req->dst_buf_list;
    result = &req->result; 
    if (likely(result->status == CPA_DC_OK)) {
        save_compressed_page_header(req->block,
                                    &req->mpa,
                                    (uint64_t)result->produced,
                                    result->checksum);
        save_compressed_data((void*)buf_list->pBuffers[0].pData, result->produced);
        compression_counters.compressed_size += result->produced;
        compression_counters.pages += req->mpa.pages;
    } else if (result->status == CPA_DC_OVERFLOW) {
        qat_dev->overflow++;
        save_uncompressed_page(req->block, &req->mpa);
    }
 
    qat_dev->responses++;
    qat_inst_req_free(req);
    return 0;
}

void qat_flush_data_compress(void)
{
    qat_req_t *req;
    if (qat_dev->responses == qat_dev->requests) {
        return;
    }
    qemu_spin_lock(&qat_dev->lock);
    qat_dev->flush_stage = true;
    while (qat_dev->req_post_num > 0) {
        req = QLIST_FIRST(&qat_dev->req_post_list);
        QLIST_REMOVE(req, node);
        qat_dev->req_post_num--;
        qat_send_req(req);
    }
    qemu_spin_unlock(&qat_dev->lock);
 
    while (qat_dev->responses != qat_dev->requests) {
        qemu_cond_timedwait(&qat_dev->cond, &qat_dev->mutex, 1);
    }
    qemu_spin_lock(&qat_dev->lock);
    qat_dev->flush_stage = false;
    qemu_spin_unlock(&qat_dev->lock);
}
void qat_flush_data_decompress(void)
{
    if (qat_dev->responses == qat_dev->requests)
        return;

    qat_dev->flush_stage = true;
    while (qat_dev->responses != qat_dev->requests)
        qemu_cond_timedwait(&qat_dev->cond, &qat_dev->mutex, 1);

    qat_dev->flush_stage = false;
}

int qat_compress_page(RAMBlock *block, MultiPageAddr *mpa)
{
    CpaStatus status;
    qat_inst_t *inst;
    qat_req_t *req;
    int ret = qat_save_zero_page(block, mpa);
    if (ret < 0) {
        error_report("%s: qat_save_zero_page failed", __func__);
        return -1;
    }
    if (mpa->pages == 0) {
        // all zero-pages
        return 0;
    }
    while (1) {
        qemu_spin_lock(&qat_dev->lock);
        if (!qat_dev->req_post_num) {
            qemu_spin_unlock(&qat_dev->lock);
            break;
        }
        req = QLIST_FIRST(&qat_dev->req_post_list);
        QLIST_REMOVE(req, node);
        qat_dev->req_post_num--;
        qemu_spin_unlock(&qat_dev->lock);

        qat_send_req(req);
    }

    inst = qat_select_inst_rr();
    req = qat_get_compress_req(inst, block, mpa);
    if (unlikely(req == NULL)) {
        error_report("%s: qat get NULL request ptr for compression!", __func__);
        return -1;
    }
    qat_dev->requests++;
    req->id = qat_dev->requests;
    req->result.checksum = 0;
    while (inst->queue_depth >= g_instReqCacheNum)
        usleep(100);
    do {
        status = cpaDcCompressData(inst->inst_handle,
                               inst->sess_handle,
                               req->src_buf_list,
                               req->dst_buf_list,
                               &req->result,
                               CPA_DC_FLUSH_FINAL,
                               req);
        if (likely(status == CPA_STATUS_SUCCESS)) {
            inst->queue_depth++;
            break;
        } else if (status == CPA_STATUS_RETRY) {
            usleep(100);
        } else {        
            error_report("%s: requests=%ld, fail to compress, status=%d",
                     __func__, qat_dev->requests, status);
            qat_inst_req_free(req);
            qat_dev->requests--;
            return -1;
        }
    } while (status == CPA_STATUS_RETRY);

    return 0;
}

int qat_decompress_page(QEMUFile *f, RAMBlock *block, int bytes,
                        MultiPageAddr *mpa, uint32_t checksum)
{
    CpaStatus status;
    if (qat_dev->state < 0) {
        error_report("%s: error state", __func__);
        return -1;
    }
    if (unlikely((block == NULL) || (bytes == 0))) {
        error_report("%s: invalid param, block=%p, bytes=%d", __func__, block, bytes);
        return -1;
    }
    qat_dev->requests++;
    qat_inst_t *inst = qat_select_inst_rr();
    qat_req_t *req = qat_get_decompress_req(inst, f, block, bytes, mpa);
    if (unlikely(req == NULL)) {
        error_report("%s: fail to get a req", __func__);
        return -1;
    }
    req->id = qat_dev->requests;
    req->checksum = checksum;
    req->result.checksum = 0;
    while (inst->queue_depth >= g_instReqCacheNum)
        usleep(100);
    do {
        status = cpaDcDecompressData(inst->inst_handle,
                                 inst->sess_handle,
                                 req->src_buf_list,
                                 req->dst_buf_list,
                                 &req->result,
                                 CPA_DC_FLUSH_FINAL,
                                 req);
        if (likely(status == CPA_STATUS_SUCCESS)) {
            inst->queue_depth++;
            return 0;
        } else if (status == CPA_STATUS_RETRY) {
            usleep(100);
        } else {
            error_report("%s: requests=%ld, fail to decompress, status=%d",
                __func__, qat_dev->requests, status);
            qat_inst_req_free_lock(req);
            return -1;
        }
    } while (status == CPA_STATUS_RETRY);

    return 0;
}

static void qat_inst_req_cache_list_cleanup(qat_inst_t *inst)
{
    qat_req_t *req = NULL, *req_next = NULL;
    QLIST_FOREACH_SAFE(req, &inst->req_cache_list, node, req_next) {
        QLIST_REMOVE(req, node);
        qat_buf_list_free(req->src_buf_list, inst->src_buf_size);
        qat_buf_list_free(req->dst_buf_list, inst->dst_buf_size);
        g_free(req);
        inst->req_cache_num--;
    }

    /* Sanity check */
    if (inst->req_cache_num) {
        error_report("%s: req_cache_num incorrect :%u", __func__, inst->req_cache_num);
    }
}

static int qat_inst_req_cache_list_setup(qat_inst_t *inst)
{
    qat_req_t *req;
    inst->req_cache_num = 0;
    QLIST_INIT(&inst->req_cache_list);

    for (int i = 0; i < g_instReqCacheNum; i++) {
        req = qat_inst_req_alloc_slow(inst);
        if (unlikely(req == NULL)) {
            error_report("%s: req pre-alloc failed", __func__);
            return -1;
        }

        QLIST_INSERT_HEAD(&inst->req_cache_list, req, node);
        inst->req_cache_num++;
    }

    return 0;
}

static int qat_inst_setup(qat_inst_t *inst, qat_setup_type_t type)
{
    CpaInstanceInfo2 inst_info;
    CpaInstanceHandle inst_handle = inst->inst_handle;
    CpaStatus status;

    status = cpaDcInstanceGetInfo2(inst_handle, &inst_info);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to get instance info, status = %x", __func__, status);
        return -1;
    }
    inst->node_affinity = inst_info.nodeAffinity;
    if (type == QAT_SETUP_DECOMPRESS) {
        inst->src_buf_num = 1;
        inst->src_buf_size = QAT_DECOMP_REQ_BUF_SIZE;
        inst->dst_buf_num = qat_dev->zero_copy ? RAM_SAVE_MAX_PAGE_NUM: 1;
        inst->dst_buf_size = qat_dev->zero_copy ? 0: QAT_DECOMP_REQ_BUF_SIZE;
    } else {
        inst->src_buf_num = qat_dev->zero_copy ? RAM_SAVE_MULTI_PAGE_NUM: 1;
        inst->src_buf_size = qat_dev->zero_copy ? 0: QAT_COMP_REQ_BUF_SIZE;
        inst->dst_buf_num = 1;
        inst->dst_buf_size = QAT_COMP_REQ_BUF_SIZE;
    }
    status = cpaDcSetAddressTranslation(inst_handle, qat_addr_translate);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: unable to set address translation", __func__);
        return -1;
    }

    status = cpaDcStartInstance(inst_handle, 0, NULL);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to start", __func__);
        return -1;
    }

    if (qat_inst_session_setup(inst, type) < 0)
        return -1;

    if (qat_inst_add_to_epoll(inst) < 0)
        return -1;

    if (qat_inst_req_cache_list_setup(inst) < 0)
        return -1;
    inst->queue_depth = 0;
    return 0;
}

static void qat_inst_cleanup(qat_inst_t *inst)
{
    CpaDcSessionHandle sess_handle = inst->sess_handle;
    CpaInstanceHandle inst_handle = inst->inst_handle;
    CpaStatus status;

    qat_inst_req_cache_list_cleanup(inst);
    /* Close the DC Session */
    status = cpaDcRemoveSession(inst_handle, sess_handle);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to remove session, status=%d", __func__, status);
        return;
    }

    status = cpaDcStopInstance(inst_handle);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to remove session, status=%d", __func__, status);
        return;
    }

    qat_mem_free((void **)&sess_handle);
}

static int check_qat_svm_status(CpaInstanceHandle inst_handle,
                                bool *svm_enabled)
{
    CpaInstanceInfo2 inst_info;
    CpaStatus status;
    status = cpaDcInstanceGetInfo2(inst_handle, &inst_info);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: cpaDcInstanceGetInfo2() failed", __func__);
        return -1;
    }
    *svm_enabled = inst_info.requiresPhysicallyContiguousMemory? false : true;
    return 0;
}

static int get_meta_buf_size(CpaInstanceHandle inst_handle,
                            uint32_t *meta_buf_size)
{
    CpaStatus status;
    status = cpaDcBufferListGetMetaSize(inst_handle, RAM_SAVE_MAX_PAGE_NUM,
                                        meta_buf_size);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: fail to get memory size for meta data", __func__);
        return -1;
    }
    return 0;
}
int qat_setup(qat_setup_type_t type)
{
    uint16_t inst_num;
    int ret, processNum, i;
    CpaStatus status;
    char ProcessNamePrefix[] = "SSL";
    char ProcessName[10] = "\0";
    if (!migrate_use_compression()) {
        return 0;
    }
    status = qaeMemInit();
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: unable to init qaeMEM", __func__);
        return -1;
    }

    for (processNum = 0; processNum < MAX_PROCESS_NUM; processNum++) {
        sprintf(ProcessName, "%s%d", ProcessNamePrefix, processNum);
        status = icp_sal_userStart(processNum ? ProcessName : ProcessNamePrefix);
        if (status == CPA_STATUS_SUCCESS) {
            break;
        }
    }

    if (processNum == MAX_PROCESS_NUM && status != CPA_STATUS_SUCCESS) {
        error_report("%s: unable to start SAL, status=%d", __func__, status);
        return -1;
    }
    qat_dev = g_malloc0(sizeof(qat_dev_t));
    qat_dev->type = type;
    qemu_spin_init(&qat_dev->lock);
    QLIST_INIT(&qat_dev->req_post_list);
    qat_dev->req_post_num = 0;
    qat_dev->flush_stage = false;
    qat_dev->state = 0;
    qemu_cond_init(&qat_dev->cond);
    qemu_mutex_init(&qat_dev->mutex);

    status = cpaDcGetNumInstances(&inst_num);
    if (unlikely((status != CPA_STATUS_SUCCESS) || (inst_num == 0))) {
        error_report("%s: no qat instance available", __func__);
        goto err_free_qat_dev;
    }
    qat_dev->inst_num = inst_num;

    qat_dev->inst_handles = g_malloc0(sizeof(CpaInstanceHandle) * inst_num);
    qat_dev->insts = g_malloc0(sizeof(qat_inst_t) * inst_num);
    status = cpaDcGetInstances(inst_num, qat_dev->inst_handles);
    if (unlikely(status != CPA_STATUS_SUCCESS)) {
        error_report("%s: unable to get instance handles", __func__);
        goto err_free_qat_dev;
    }

    // Here we only check the first instance for simplicity. System administrator
    // should make sure all instances have the same configuration.
    ret = check_qat_svm_status(qat_dev->inst_handles[0], &qat_dev->svm_enabled);
    if (unlikely(ret != 0)) {
        error_report("%s: failed to check qat svm status", __func__);
        goto err_free_qat_dev;
    }

    if (qat_dev->svm_enabled) {
        qat_dev->zero_copy = true;
        qat_mem_alloc = qat_mem_alloc_virt;
        qat_mem_free = qat_mem_free_virt;
        qat_addr_translate = qat_virt_to_phy_svm1;
    } else {
        qat_dev->zero_copy = false;
        qat_mem_alloc = qat_mem_alloc_phy;
        qat_mem_free = qat_mem_free_phy;
        qat_addr_translate = qat_virt_to_phy_svm0;
    }

    ret = get_meta_buf_size(qat_dev->inst_handles[0], &qat_dev->meta_buf_size);
    if (unlikely(ret != 0)) {
        error_report("%s: unable to get instance handles", __func__);
        goto err_free_qat_dev;
    }

    qat_dev->efd = epoll_create1(0);
    if (unlikely(qat_dev->efd < 0)) {
        error_report("%s: fail to create epoll fd", __func__);
        goto err_free_qat_dev;
    }
    epoll_thread_running = true;

    for (i = 0; i < inst_num; i++) {
        qat_dev->insts[i].id = i;
        qat_dev->insts[i].inst_handle = qat_dev->inst_handles[i];
        ret = qat_inst_setup(&qat_dev->insts[i], type);
        if (unlikely(ret != 0)) {
            goto err_inst_cleanup;
        }
    }
    qemu_thread_create(&qat_dev->epoll_thread, "qat_epoll_thread",
                       qat_epoll_thread_run, qat_dev, QEMU_THREAD_JOINABLE);
    if (unlikely(ret != 0)) {
        goto err_inst_cleanup;
    }

    info_report("%s: section=SSL%d, inst_num=%d, zero_copy=%d",
            __func__, processNum, inst_num, qat_dev->zero_copy);
    info_report("%s: cache_req_num=%d, MULTI_PAGE_NUM=%d, MAX_PAGE_NUM=%d",
            __func__, g_instReqCacheNum, RAM_SAVE_MULTI_PAGE_NUM, RAM_SAVE_MAX_PAGE_NUM);
    return 0;
err_inst_cleanup:
    while (i >= 0) {
        if (qat_dev->insts[i].inst_handle)
            qat_inst_cleanup(&qat_dev->insts[i]);
        i--;
    }

err_free_qat_dev:
    if (qat_dev) {
        if (qat_dev->inst_handles)
            g_free(qat_dev->inst_handles);
        if (qat_dev->insts)
            g_free(qat_dev->insts);
        if (qat_dev->efd)
            close(qat_dev->efd);
        qemu_cond_destroy(&qat_dev->cond);
        qemu_mutex_destroy(&qat_dev->mutex);
        g_free(qat_dev);
    }
    return -1;
}

void qat_cleanup(void)
{
    if (!migrate_use_compression())
        return;
    if (unlikely(qat_dev == NULL))
        return;

    while (likely(qat_dev->responses != qat_dev->requests)) {
        cpu_relax();
    }

    epoll_thread_running = false;
    qemu_thread_join(&qat_dev->epoll_thread);
    info_report("%s: requests=%ld, responses=%ld, overflow=%ld",
             __func__, qat_dev->requests, qat_dev->responses, qat_dev->overflow);
    close(qat_dev->efd);

    while (qat_dev->inst_num) {
        qat_inst_cleanup(&qat_dev->insts[--qat_dev->inst_num]);
    }
    g_free(qat_dev->inst_handles);
    g_free(qat_dev->insts);
    qemu_cond_destroy(&qat_dev->cond);
    qemu_mutex_destroy(&qat_dev->mutex);
    g_free(qat_dev);
    qat_dev = NULL;
    icp_sal_userStop();
    qaeMemDestroy();
}
