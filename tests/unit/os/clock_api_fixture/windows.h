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

#endif  // XR_TEST_WINDOWS_CLOCK_API_H
