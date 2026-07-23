#ifndef XR_FIXTURE_NATIVE_OUTPUT_H
#define XR_FIXTURE_NATIVE_OUTPUT_H

#include <stdint.h>

typedef struct CValue {
    int64_t value;
} CValue;

int32_t xr_fixture_fill_i64(void *out);

#endif
