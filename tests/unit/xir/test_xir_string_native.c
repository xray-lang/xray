/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_native.c - Native string results without VM or compiler dependencies
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */

#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_string_cases.h"
XR_DATA const XrXirCallEntry fixture_strings0_entries[3];
XR_DATA const XrXirCallEntry fixture_strings1_entries[3];
int main(void) {
    const XrXirCallEntry *tables[] = {fixture_strings0_entries, fixture_strings1_entries};
    for (uint32_t variant = 0; variant < 2; ++variant) for (uint32_t mode = 0; mode < 9; ++mode) {
        XrXirValue value = string_cases(tables[variant], variant, mode);
        if (value.type == XR_XIR_STRING) string_bytes(&value, string_expected, sizeof(string_expected) - 1);
        xr_xir_value_drop(&value);
    }
    puts("Native Unicode string calls, output, suspension, failure and owned results passed");
    return 0;
}
