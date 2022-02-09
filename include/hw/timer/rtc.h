/*
* QEMU rtc
*
* We introduce the functions here to decouple for the upstream
* include/hw/timer/mc146818rtc.h
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

#ifndef INCLUDE_HW_TIMER_RTC_H
#define INCLUDE_HW_TIMER_RTC_H

#include <stdlib.h>
#include <linux/types.h>
#include <linux/kvm.h>
#include "qemu-log.h"

/* flags to control coalesced irq */
#define KVM_GET_RTC_IRQ_COALESCED        (1 << 0)
#define KVM_SET_RTC_IRQ_COALESCED        (1 << 1)
#define KVM_RTC_LOST_TICK_POLICY_SLEW    (1 << 2)
#define KVM_SET_RTC_CATCHUP_SPEED        (1 << 3)

/* RTC is emulated in qemu, but the colasced irqs are reinjected in kvm */
#define KVM_CAP_RTC_IRQ_COALESCED 163
#define KVM_RTC_REINJECT_CONTROL _IOWR(KVMIO, 0x56, struct kvm_rtc_reinject_control)

struct kvm_rtc_reinject_control {
    __u32 rtc_irq_coalesced;
    __u8 flag;
    __u8 speed;
    __u8 reserved[30];
};

extern bool kvm_rtc_reinject_enable;

uint32_t rtc_get_coalesced_irq(void);
void rtc_set_coalesced_irq(uint32_t nr_irqs);
void rtc_lost_tick_policy_slew(void);
uint32_t rtc_catchup_speed(void);
void set_rtc_catchup_speed(const uint32_t speed);

#endif
