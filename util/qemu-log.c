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

#include "qemu-log.h"
#include <unistd.h>
#include "qemu/osdep.h"

#define BEGIN_YEAR 1900
#define DEFAULT_SECFRACPRECISION 6
#define LOG_LEVEL_NAME_MAX_LEN 10

static const int tenPowers[6] = { 1, 10, 100, 1000, 10000, 100000 };

typedef char intTiny;

struct syslogTime {
        intTiny timeType;       /* 0 - unitinialized
                                 * 1 - RFC 3164
                                 * 2 - syslog-protocol
                                 */
        intTiny month;
        intTiny day;
        intTiny hour;           /* 24 hour clock */
        intTiny minute;
        intTiny second;
        intTiny secfracPrecision;
        intTiny OffsetMinute;   /* UTC offset in minutes */
        intTiny OffsetHour;     /* UTC offset in hours
                                 * full UTC offset minutes
                                 *           = OffsetHours*60 + OffsetMinute.
                                 * Then use OffsetMode to know the direction.
                                 */
        char OffsetMode;        /* UTC offset + or - */
        short year;
        int secfrac;    /* fractional seconds (must be 32 bit!) */
        intTiny inUTC;  /* forced UTC? */
};
typedef struct syslogTime syslogTime_t;

typedef struct syslogName_s {
        const char *c_name;
        int c_val;
} syslogName_t;

static syslogName_t syslogPriNames[] = {
        {"emerg",   LOG_EMERG},
        {"alert",   LOG_ALERT},
        {"crit",    LOG_CRIT},
        {"err",     LOG_ERR},
        {"warning", LOG_WARNING},
        {"notice",  LOG_NOTICE},
        {"info",    LOG_INFO},
        {"debug",   LOG_DEBUG},
        {NULL,      -1}
};

/**
 * Format a syslogTimestamp to a RFC3339 timestamp string (as
 * specified in syslog-protocol).
 *
 * Notes: rfc_time_buf size >= TIMESTAMP_MAX_LEN
 */
static void formatTimestamp3339(struct syslogTime *ts, char *rfc_time_buf)
{
    int iBuf = 0;
    int power = 0;
    int secfrac = 0;
    short digit = 0;
    char *pBuf = rfc_time_buf;

    pBuf[iBuf++] = (ts->year / 1000) % 10 + '0';
    pBuf[iBuf++] = (ts->year / 100) % 10 + '0';
    pBuf[iBuf++] = (ts->year / 10) % 10 + '0';
    pBuf[iBuf++] = ts->year % 10 + '0';
    pBuf[iBuf++] = '-';
    /* month */
    pBuf[iBuf++] = (ts->month / 10) % 10 + '0';
    pBuf[iBuf++] = ts->month % 10 + '0';
    pBuf[iBuf++] = '-';
    /* day */
    pBuf[iBuf++] = (ts->day / 10) % 10 + '0';
    pBuf[iBuf++] = ts->day % 10 + '0';
    pBuf[iBuf++] = 'T';
    /* hour */
    pBuf[iBuf++] = (ts->hour / 10) % 10 + '0';
    pBuf[iBuf++] = ts->hour % 10 + '0';
    pBuf[iBuf++] = ':';
    /* minute */
    pBuf[iBuf++] = (ts->minute / 10) % 10 + '0';
    pBuf[iBuf++] = ts->minute % 10 + '0';
    pBuf[iBuf++] = ':';
    /* second */
    pBuf[iBuf++] = (ts->second / 10) % 10 + '0';
    pBuf[iBuf++] = ts->second % 10 + '0';

    if (ts->secfracPrecision > 0) {
        pBuf[iBuf++] = '.';
        power = tenPowers[(ts->secfracPrecision - 1) % 6];
        secfrac = ts->secfrac;
        while (power > 0) {
            digit = secfrac / power;
            secfrac -= digit * power;
            power /= 10;
            pBuf[iBuf++] = digit + '0';
        }
    }

    pBuf[iBuf++] = ts->OffsetMode;
    pBuf[iBuf++] = (ts->OffsetHour / 10) % 10 + '0';
    pBuf[iBuf++] = ts->OffsetHour % 10 + '0';
    pBuf[iBuf++] = ':';
    pBuf[iBuf++] = (ts->OffsetMinute / 10) % 10 + '0';
    pBuf[iBuf++] = ts->OffsetMinute % 10 + '0';

    pBuf[iBuf] = '\0';
}

void qemu_convert_timestamp(struct timeval tp, char *buf, int buf_size)
{
    struct tm *tm;
    struct tm tmBuf;
    long lBias = 0;
    time_t secs;
    syslogTime_t ts;
    char rfc_time_buf[TIMESTAMP_MAX_LEN] = {0};

   /* RFC 3339 timestamp length must be greater than or equal 33 */
    if (buf_size < TIMESTAMP_MAX_LEN) {
        buf[0] = '\0';
        (void)printf("RFC 3339 timestamp length must be greater than or equal 33\n");
        return;
    }

    secs = tp.tv_sec;
    tm = localtime_r(&secs, &tmBuf);

    ts.year = tm->tm_year + BEGIN_YEAR;
    ts.month = tm->tm_mon + 1;
    ts.day = tm->tm_mday;
    ts.hour = tm->tm_hour;
    ts.minute = tm->tm_min;
    ts.second = tm->tm_sec;
    ts.secfrac = tp.tv_usec;
    ts.secfracPrecision = DEFAULT_SECFRACPRECISION;

    lBias = tm->tm_gmtoff;
    if (lBias < 0) {
        ts.OffsetMode = '-';
        lBias *= -1;
    } else {
        ts.OffsetMode = '+';
    }

    ts.OffsetHour = lBias / 3600;
    ts.OffsetMinute = (lBias % 3600) / 60;

    formatTimestamp3339(&ts, rfc_time_buf);
    (void)snprintf(buf, buf_size, "%s", rfc_time_buf);
}

void qemu_get_timestamp(char *buf, int buf_size)
{
    struct timeval tp;
    (void)gettimeofday(&tp, NULL);

    qemu_convert_timestamp(tp, buf, buf_size);
}


static void qemu_get_loglevelname(int level, char *log_level_name, int len)
{
    syslogName_t *c;

    for (c = syslogPriNames; c->c_name; c++) {
        if (level == c->c_val) {
            (void)snprintf(log_level_name, len, "%s", c->c_name);
            return;
        }
    }

    (void)printf("The log level is wrong\n");
}

void qemu_log_print(int level, const char *funcname, int linenr,
                    const char *fmt, ...)
{
    va_list ap;
    char time_buf[TIMESTAMP_MAX_LEN] = {0};
    char log_level_name[LOG_LEVEL_NAME_MAX_LEN] = {0};
    char buf[1024] = {0};

    qemu_get_timestamp(time_buf, TIMESTAMP_MAX_LEN);
    qemu_get_loglevelname(level, log_level_name, sizeof(log_level_name));

    va_start(ap, fmt);
    (void)vsnprintf(buf, 1024, fmt, ap);
    va_end(ap);

    if (funcname != NULL) {
        (void)fprintf(stderr, "%s|%s|qemu[%d]|[%d]|%s[%d]|: %s", time_buf,
                      log_level_name, getpid(), qemu_get_thread_id(),
                      funcname, linenr, buf);
    } else {
        (void)fprintf(stderr, "%s|%s|qemu[%d]|[%d]|%s", time_buf, log_level_name,
                      getpid(), qemu_get_thread_id(), buf);
    }
}
