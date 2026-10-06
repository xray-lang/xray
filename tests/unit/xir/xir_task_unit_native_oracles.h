/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef XIR_TASK_UNIT_NATIVE_ORACLES_H
#define XIR_TASK_UNIT_NATIVE_ORACLES_H
#include "xir/xxir_enum.h"
typedef struct UnitNativeOracle { const char *name, *entry; bool escaped, unit_root, error; unsigned outputs; } UnitNativeOracle;
static const UnitNativeOracle unit_native_oracles[] = {
    {"inferred", "main", false, false, false, 1}, {"explicit_unit", "main", false, false, false, 1},
    {"explicit_null", "main", false, false, false, 1}, {"generic", "main", false, false, false, 1},
    {"escape", "handle", true, false, false, 1}, {"unit_root", "main", false, true, false, 1},
    {"unknown_waiter", "main", false, false, false, 1}, {"escaped_error", "handle", true, false, true, 0}};
static inline void unit_native_empty(const XrXirValue *value) { CHECK(!value->type && !value->reserved && !value->payload); }
static inline void unit_native_outcome(const XrXirCallResult *outcome, bool error) {
    CHECK(outcome->status == (error ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED) && !outcome->wake);
    CHECK(xr_xir_panic_empty(&outcome->panic));
    if (!error) { unit_native_empty(&outcome->value); return; }
    XrXirValue concrete = outcome->value;
    if (concrete.type == XR_XIR_ERROR) CHECK(xr_xir_error_borrow(&outcome->value, &concrete));
    XrXirEnumBorrow value = {0}; CHECK(xr_xir_enum_borrow(&concrete, &value) == XR_XIR_VALUE_OK);
    CHECK(value.name.length == 7 && !memcmp(value.name.bytes, "Problem", 7) && value.member.length == 6 &&
        !memcmp(value.member.bytes, "Failed", 6) && value.field_count == 1);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&value.fields[0], &bytes, &length) && length == 5 && !memcmp(bytes, "owned", 5));
}
#endif
