/*
 * Copyright (c) 2021, 2022 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "channel.h"
#include "file.h"
#include "migration.h"
#include "io/channel-file.h"
#include "io/channel-util.h"
#include "trace.h"
#include "sysemu/runstate.h"
#include <unistd.h>

void file_start_outgoing_migration(MigrationState *s, const char *filename,
                                   Error **errp)
{
    g_autoptr(QIOChannelFile) fioc = NULL;
    QIOChannel *ioc;
    pid_t pid;
    char *filename_p;

    pid = getpid();
    filename_p = g_strdup_printf("%s.%d", filename, pid);
    trace_migration_file_outgoing(filename_p);

    fioc = qio_channel_file_new_path(filename_p, O_CREAT | O_WRONLY | O_TRUNC,
                                     0600, errp);
    if (!fioc) {
        g_free(filename_p);
        return;
    }

    ioc = QIO_CHANNEL(fioc);
    qio_channel_set_name(ioc, "migration-file-outgoing");
    migration_channel_connect(s, ioc, NULL, NULL);
    g_free(filename_p);
}

static void file_migrate_complete_unlink_file(void *opaque)
{
    char *filename = opaque;
    unlink(filename);
    g_free(filename);
}

static gboolean file_accept_incoming_migration(QIOChannel *ioc,
                                               GIOCondition condition,
                                               gpointer opaque)
{
    migration_channel_process_incoming(ioc);
    object_unref(OBJECT(ioc));
    return G_SOURCE_REMOVE;
}

void file_start_incoming_migration(const char *filename, Error **errp)
{
    QIOChannelFile *fioc = NULL;
    QIOChannel *ioc;
    pid_t pid;
    char *filename_p;

    pid = getpid();
    filename_p = g_strdup_printf("%s.%d", filename, pid);
    trace_migration_file_incoming(filename_p);


    fioc = qio_channel_file_new_path(filename_p, O_RDONLY, 0, errp);
    if (!fioc) {
        g_free(filename_p);
        return;
    }

    ioc = QIO_CHANNEL(fioc);
    qio_channel_set_name(QIO_CHANNEL(ioc), "migration-file-incoming");
    qio_channel_add_watch_full(ioc, G_IO_IN,
                               file_accept_incoming_migration,
                               NULL, NULL,
                               g_main_context_get_thread_default());

    /*
     * Register Handler to delete VM state save file when
     * qemu live update complete
     */
    qemu_add_cpr_exec_complete_handler(file_migrate_complete_unlink_file,
                                       (void *)filename_p);
}
