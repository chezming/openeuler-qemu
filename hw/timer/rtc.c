 /*
 * QEMU rtc
 *
 * We introduce the functions here to decouple for the upstream
 * hw/timer/mc146818rtc.c
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

#include <stdlib.h>
#include "qemu/osdep.h"
#include "hw/timer/rtc.h"
#include "sysemu/kvm.h"
#include "qemu/config-file.h"
#include "qemu/option.h"

extern int kvm_vm_ioctl(KVMState *s, int type, ...);

uint32_t rtc_get_coalesced_irq(void)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    control.flag = KVM_GET_RTC_IRQ_COALESCED;
    ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
    if (ret < 0) {
        QEMU_LOG(LOG_ERR, "Failed to get coalesced irqs from kmod: %d\n", ret);
    }
    return control.rtc_irq_coalesced;
}

void rtc_set_coalesced_irq(uint32_t nr_irqs)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    control.rtc_irq_coalesced = nr_irqs;
    control.flag = KVM_SET_RTC_IRQ_COALESCED;
    ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
    if (ret < 0) {
        QEMU_LOG(LOG_ERR, "Failed to set coalesced irqs to kmod: %d, %u\n", ret, nr_irqs);
    }
}

void rtc_lost_tick_policy_slew(void)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    control.flag = KVM_RTC_LOST_TICK_POLICY_SLEW;
    ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
    if (ret < 0) {
        QEMU_LOG(LOG_ERR, "Failed to notify kvm to use lost tick policy slew: %d\n", ret);
    }
}

uint32_t rtc_catchup_speed(void)
{
    uint32_t speed;
    QemuOpts *opts = qemu_find_opts_singleton("rtc");

    speed = qemu_opt_get_number(opts, "speed", 0);
    QEMU_LOG(LOG_INFO, "rtc catchup speed: %u\n", speed);

    return speed;
}

void set_rtc_catchup_speed(const uint32_t speed)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    if (speed > 0) {
        control.flag = KVM_SET_RTC_CATCHUP_SPEED;
        control.speed = speed;
        ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
        if (ret < 0) {
            QEMU_LOG(LOG_ERR, "Failed to set rtc_catchup_speed: %d\n", ret);
        }
        QEMU_LOG(LOG_INFO, "Success to set rtc_catchup_speed: %u\n", speed);
    }
}
