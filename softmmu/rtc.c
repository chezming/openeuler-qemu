/*
 * RTC configuration and clock read
 *
 * Copyright (c) 2003-2020 QEMU contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "sysemu/replay.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "qemu/log.h"
#include "qemu/config-file.h"

static enum {
    RTC_BASE_UTC,
    RTC_BASE_LOCALTIME,
    RTC_BASE_DATETIME,
} rtc_base_type = RTC_BASE_UTC;
static time_t rtc_ref_start_datetime;
static int rtc_realtime_clock_offset; /* used only with QEMU_CLOCK_REALTIME */
static int rtc_host_datetime_offset = -1; /* valid & used only with
                                             RTC_BASE_DATETIME */
static time_t rtc_date_diff = 0;
QEMUClockType rtc_clock;
/***********************************************************/
/* RTC reference time/date access */
static time_t qemu_ref_timedate(QEMUClockType clock)
{
    time_t value = qemu_clock_get_ms(clock) / 1000;
    switch (clock) {
    case QEMU_CLOCK_REALTIME:
        value -= rtc_realtime_clock_offset;
        /* fall through */
    case QEMU_CLOCK_VIRTUAL:
        value += rtc_ref_start_datetime;
        break;
    case QEMU_CLOCK_HOST:
        if (rtc_base_type == RTC_BASE_DATETIME) {
            value -= rtc_host_datetime_offset;
        }
        break;
    default:
        assert(0);
    }
    return value;
}

void qemu_get_timedate(struct tm *tm, int offset)
{
    time_t ti = qemu_ref_timedate(rtc_clock);

    ti += offset;

    switch (rtc_base_type) {
    case RTC_BASE_DATETIME:
    case RTC_BASE_UTC:
        gmtime_r(&ti, tm);
        break;
    case RTC_BASE_LOCALTIME:
        localtime_r(&ti, tm);
        break;
    }
}

time_t qemu_timedate_diff(struct tm *tm)
{
    time_t seconds;

    switch (rtc_base_type) {
    case RTC_BASE_DATETIME:
    case RTC_BASE_UTC:
        seconds = mktimegm(tm);
        break;
    case RTC_BASE_LOCALTIME:
    {
        struct tm tmp = *tm;
        tmp.tm_isdst = -1; /* use timezone to figure it out */
        seconds = mktime(&tmp);
        break;
    }
    default:
        abort();
    }

    return seconds - qemu_ref_timedate(QEMU_CLOCK_HOST);
}

time_t get_rtc_date_diff(void)
{
    return rtc_date_diff;
}

void set_rtc_date_diff(time_t diff)
{
    rtc_date_diff = diff;
}

static void configure_rtc_base_datetime(const char *startdate)
{
    time_t rtc_start_datetime;
    struct tm tm;

    if (sscanf(startdate, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon,
               &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        /* OK */
    } else if (sscanf(startdate, "%d-%d-%d",
                      &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
    } else {
        goto date_fail;
    }
    tm.tm_year -= 1900;
    tm.tm_mon--;
    rtc_start_datetime = mktimegm(&tm);
    if (rtc_start_datetime == -1) {
    date_fail:
        error_report("invalid datetime format");
        error_printf("valid formats: "
                     "'2006-06-17T16:01:21' or '2006-06-17'\n");
        exit(1);
    }
    rtc_host_datetime_offset = rtc_ref_start_datetime - rtc_start_datetime;
    rtc_ref_start_datetime = rtc_start_datetime;
}

void configure_rtc(QemuOpts *opts)
{
    const char *value;

    /* Set defaults */
    rtc_clock = QEMU_CLOCK_HOST;
    rtc_ref_start_datetime = qemu_clock_get_ms(QEMU_CLOCK_HOST) / 1000;
    rtc_realtime_clock_offset = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) / 1000;

    value = qemu_opt_get(opts, "base");
    if (value) {
        if (!strcmp(value, "utc")) {
            rtc_base_type = RTC_BASE_UTC;
        } else if (!strcmp(value, "localtime")) {
            Error *blocker = NULL;
            rtc_base_type = RTC_BASE_LOCALTIME;
            error_setg(&blocker, QERR_REPLAY_NOT_SUPPORTED,
                      "-rtc base=localtime");
            replay_add_blocker(blocker);
        } else {
            rtc_base_type = RTC_BASE_DATETIME;
            configure_rtc_base_datetime(value);
        }
    }
    value = qemu_opt_get(opts, "clock");
    if (value) {
        if (!strcmp(value, "host")) {
            rtc_clock = QEMU_CLOCK_HOST;
        } else if (!strcmp(value, "rt")) {
            rtc_clock = QEMU_CLOCK_REALTIME;
        } else if (!strcmp(value, "vm")) {
            rtc_clock = QEMU_CLOCK_VIRTUAL;
        } else {
            error_report("invalid option value '%s'", value);
            exit(1);
        }
    }
    value = qemu_opt_get(opts, "driftfix");
    if (value) {
        if (!strcmp(value, "slew")) {
            object_register_sugar_prop("mc146818rtc",
                                       "lost_tick_policy",
                                       "slew",
                                       false);
        } else if (!strcmp(value, "none")) {
            /* discard is default */
        } else {
            error_report("invalid option value '%s'", value);
            exit(1);
        }
    }
}

uint32_t rtc_get_coalesced_irq(void)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    control.flag = KVM_GET_RTC_IRQ_COALESCED;
    ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
    if (ret < 0) {
        qemu_log("Failed to get coalesced irqs from kmod: %d\n", ret);
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
        qemu_log("Failed to set coalesced irqs to kmod: %d, %u\n", ret, nr_irqs);
    }
}

void rtc_lost_tick_policy_slew(void)
{
    struct kvm_rtc_reinject_control control = {};
    int ret;

    control.flag = KVM_RTC_LOST_TICK_POLICY_SLEW;
    ret = kvm_vm_ioctl(kvm_state, KVM_RTC_REINJECT_CONTROL, &control);
    if (ret < 0) {
        qemu_log("Failed to notify kvm to use lost tick policy slew: %d\n", ret);
    }
}

uint32_t rtc_catchup_speed(void)
{
    uint32_t speed;
    QemuOpts *opts = qemu_find_opts_singleton("rtc");

    speed = qemu_opt_get_number(opts, "speed", 0);
    qemu_log("rtc catchup speed: %u\n", speed);

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
            qemu_log("Failed to set rtc_catchup_speed: %d\n", ret);
            return;
        }
        qemu_log("Success to set rtc_catchup_speed: %u\n", speed);
    }
}
