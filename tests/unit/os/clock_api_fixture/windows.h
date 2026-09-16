/* Minimal injected API for executing the Windows clock implementation. */
#ifndef XR_TEST_WINDOWS_CLOCK_API_H
#define XR_TEST_WINDOWS_CLOCK_API_H

#include <stdint.h>

typedef int BOOL;
typedef uint32_t DWORD;
typedef struct {
    int64_t QuadPart;
} LARGE_INTEGER;
typedef union {
    struct {
        uint32_t LowPart;
        uint32_t HighPart;
    };
    uint64_t QuadPart;
} ULARGE_INTEGER;
typedef struct {
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
} FILETIME;
#define MAXDWORD UINT32_MAX

static BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
static BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);
static void GetSystemTimePreciseAsFileTime(FILETIME *value);
static void *GetCurrentProcess(void);
static BOOL GetProcessTimes(void *process, FILETIME *creation, FILETIME *exit, FILETIME *kernel,
                            FILETIME *user);
static void Sleep(DWORD milliseconds);

#endif  // XR_TEST_WINDOWS_CLOCK_API_H
