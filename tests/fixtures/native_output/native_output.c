#include "native_output.h"

int32_t xr_fixture_fill_i64(void *out) {
    if (!out)
        return 1;
    ((CValue *) out)->value = INT64_C(42);
    return 0;
}
