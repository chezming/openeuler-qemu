/*
 * Introduce QEMU_LOG
 *
 * LOG: Introduce QEMU_LOG.
 *
 * Copyright (c) 2017-2020 HUAWEI TECHNOLOGIES CO.,LTD.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UVP_QEMU_COMMON_H
#define UVP_QEMU_COMMON_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/time.h>
#include <syslog.h>
#include "qemu/typedefs.h"
#include "qemu/osdep.h"

#define TIMESTAMP_MAX_LEN  33 /* RFC 3339 timestamp length shuold be 33 */

void qemu_get_timestamp(char *buf, int buf_size);
void qemu_convert_timestamp(struct timeval tp, char *buf, int buf_size);
void qemu_log_print(int level, const char *funcname, int linenr,
                    const char *fmt, ...);

#define QEMU_LOG(level, format, ...) \
         qemu_log_print(level, __func__, __LINE__, format, ##__VA_ARGS__)

#endif
