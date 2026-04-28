/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xposix_compat.h - POSIX function shims for MSVC/Windows
 *
 * KEY CONCEPT:
 *   Provides inline implementations of common POSIX functions that
 *   MSVC does not ship. Include this header where needed; on
 *   non-MSVC compilers all definitions are no-ops (the real
 *   functions are already available via libc).
 */

#ifndef XPOSIX_COMPAT_H
#define XPOSIX_COMPAT_H

#include "xplatform.h"

#if defined(XR_COMPILER_MSVC)

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ========== localtime_r ========== */

static inline struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (localtime_s(result, timep) != 0)
        return NULL;
    return result;
}

/* ========== strndup ========== */

static inline char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *copy = (char *) malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

/* ========== getline ========== */

static inline ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream)
        return -1;
    char *buf = *lineptr;
    size_t cap = *n;
    size_t len = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (len + 2 > cap) {
            cap = (cap < 128) ? 128 : cap * 2;
            char *tmp = (char *) realloc(buf, cap);
            if (!tmp)
                return -1;
            buf = tmp;
        }
        buf[len++] = (char) c;
        if (c == '\n')
            break;
    }
    if (len == 0)
        return -1;
    buf[len] = '\0';
    *lineptr = buf;
    *n = cap;
    return (ssize_t) len;
}

/* ========== memmem ========== */

static inline void *memmem(const void *haystack, size_t haystacklen, const void *needle,
                           size_t needlelen) {
    if (needlelen == 0)
        return (void *) haystack;
    if (haystacklen < needlelen)
        return NULL;
    const unsigned char *h = (const unsigned char *) haystack;
    const unsigned char *n = (const unsigned char *) needle;
    size_t limit = haystacklen - needlelen;
    for (size_t i = 0; i <= limit; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return (void *) (h + i);
    }
    return NULL;
}

/* ========== strptime (minimal: %a, %d %b %Y %H:%M:%S %Z) ========== */

static inline char *strptime(const char *s, const char *fmt, struct tm *tm) {
    (void) fmt;
    // Minimal cookie-date parser: "Thu, 01 Jan 2026 00:00:00 GMT"
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // Skip day-of-week
    const char *p = strchr(s, ',');
    if (p)
        p += 2;
    else
        p = s;
    int day, year, hour, min, sec;
    char mon[4] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon, &year, &hour, &min, &sec) != 6)
        return NULL;
    tm->tm_mday = day;
    tm->tm_year = year - 1900;
    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = sec;
    tm->tm_mon = -1;
    for (int i = 0; i < 12; i++) {
        if (_strnicmp(mon, months[i], 3) == 0) {
            tm->tm_mon = i;
            break;
        }
    }
    if (tm->tm_mon < 0)
        return NULL;
    return (char *) (p + strlen(p));
}

/* ========== timegm ========== */

static inline time_t timegm(struct tm *tm) {
    return _mkgmtime(tm);
}

/* ========== poll (minimal: writable check for connect) ========== */

#include <winsock2.h>

static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    return WSAPoll(fds, nfds, timeout);
}

#endif  // XR_COMPILER_MSVC
#endif  // XPOSIX_COMPAT_H
