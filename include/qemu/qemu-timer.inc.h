/*
 * Introduce qemu-timer function
 *
 * We introduce the functions here to decouple for the upstream
 * include/qemu/timer.h
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

#ifndef HUAWEI_INCLUDE_QEMU_QEMU_TIMER_INC_H
#define HUAWEI_INCLUDE_QEMU_QEMU_TIMER_INC_H

#include "qemu/timer.h"

void qemu_clock_trigger_reset(QEMUClockType type);
void qemu_clock_disable_reset(void);

#endif /* end of include guard: HUAWEI_INCLUDE_QEMU_QEMU_TIMER_INC_H */
