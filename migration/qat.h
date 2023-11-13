#ifndef QEMU_MIGRATION_QAT_H
#define QEMU_MIGRATION_QAT_H

#include "ram.h"

typedef enum _qat_setup_type_t {
    QAT_SETUP_COMPRESS = 0,
    QAT_SETUP_DECOMPRESS = 1,
    QAT_SETUP_MAX,
} qat_setup_type_t;

int qat_setup(qat_setup_type_t type);
void qat_cleanup(void);
int qat_compress_page(RAMBlock *block, MultiPageAddr *mpa);
int qat_decompress_page(QEMUFile *f, RAMBlock *block, int bytes,
                        MultiPageAddr *mpa, uint32_t checksum);
void qat_flush_data_compress(void);
void qat_flush_data_decompress(void);
void *qat_epoll_thread_run(void *arg);
#endif
