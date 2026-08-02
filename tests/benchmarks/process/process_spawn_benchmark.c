/* Measure the direct inherited-stdio xr_proc_spawn/xr_proc_wait path. */

#include "os/os_proc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t) (((long double) counter.QuadPart * 1000000000.0L) /
                       (long double) frequency.QuadPart);
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t) value.tv_sec * 1000000000u + (uint64_t) value.tv_nsec;
#endif
}

static int spawn_once(void) {
#ifdef _WIN32
    const char *const argv[] = {"cmd.exe", "/d", "/c", "exit", "/b", "0", NULL};
    const char *program = "cmd.exe";
#else
    const char *const argv[] = {"/bin/sh", "-c", "true", NULL};
    const char *program = "/bin/sh";
#endif
    XrProcId child = xr_proc_spawn(program, argv);
    int exit_code = -1;
    return child != XR_PROC_INVALID && xr_proc_wait(child, &exit_code) == 0 && exit_code == 0;
}

int main(int argc, char **argv) {
    size_t samples = 200;
    if (argc == 3 && strcmp(argv[1], "--samples") == 0) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (!end || *end != '\0' || parsed == 0 || parsed > 100000)
            return 2;
        samples = (size_t) parsed;
    } else if (argc != 1) {
        return 2;
    }
    for (size_t i = 0; i < 20; i++)
        if (!spawn_once())
            return 3;
    for (size_t i = 0; i < samples; i++) {
        uint64_t start = now_ns();
        if (!spawn_once())
            return 3;
        printf("%llu\n", (unsigned long long) (now_ns() - start));
    }
    return 0;
}
