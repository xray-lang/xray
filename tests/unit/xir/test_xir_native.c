/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_native.c - Scalar execution verification
 *
 * KEY CONCEPT:
 *   Native literal expectations link only the scalar runtime and generated C.
 */

#include "xir_execution_cases.h"
#define DECLARE(n) XR_FUNC XrXirRunStatus fixture_f##n(XrXirRunContext *, const XrXirValue *, uint32_t, XrXirValue *);
DECLARE(0) DECLARE(1) DECLARE(2) DECLARE(3) DECLARE(4)
DECLARE(5) DECLARE(6) DECLARE(7) DECLARE(8)
#undef DECLARE
static XrXirRunStatus run(void *owner, uint32_t function, XrXirRunContext *context,
                         const XrXirValue *arguments, uint32_t count, XrXirValue *result) {
    const XrXirLeafEntry entries[] = {fixture_f0, fixture_f1, fixture_f2, fixture_f3,
        fixture_f4, fixture_f5, fixture_f6, fixture_f7, fixture_f8};
    (void) owner;
    CHECK(function < 9);
    return entries[function](context, arguments, count, result);
}
int main(void) {
    execution_cases(run, NULL);
    puts("XIR native scalar expectations passed");
    return 0;
}
