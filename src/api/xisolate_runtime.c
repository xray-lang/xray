/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_runtime.c - Runtime prelude enum registration
 *
 * Defines the shared VM prelude enum registry consumed by full isolate
 * construction and AOT through the immutable builtin declaration registry.
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/class/xenum.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../base/xnumber_parse_error.h"
#include "../base/xbuiltin_enum.h"

#include <string.h>

static void isolate_bind_builtin(XrVMRuntime *isolate, int32_t index, XrValue value) {
    if (!isolate || index < 0 || index >= XR_USER_GLOBALS_START)
        return;
    isolate->vm.builtins[index] = value;
    xr_runtime_core_set_builtin(xr_isolate_get_runtime_core(isolate), index, value);
}

static bool runtime_number_parse_error_is_exact(const XrEnumType *type) {
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    if (!row)
        return false;
    if (!type || !type->layout || type->layout->layout_id != row->enum_layout_id ||
        type->member_count != XR_NUMBER_PARSE_ERROR_MEMBER_COUNT)
        return false;
    for (uint32_t i = 0; i < XR_NUMBER_PARSE_ERROR_MEMBER_COUNT; i++)
        if (!type->members[i].name || strcmp(type->members[i].name, row->members[i]) != 0)
            return false;
    return true;
}

bool xr_isolate_register_runtime_prelude_enums(XrVMRuntime *isolate) {
    if (!isolate)
        return false;
    size_t count = 0;
    const XrBuiltinEnumRow *rows = xr_builtin_enum_registry(&count);
    for (size_t i = 0; i < count; i++) {
        const XrBuiltinEnumRow *row = &rows[i];
        if (row->builtin_index < 0 || row->builtin_index >= XR_USER_GLOBALS_START ||
            row->member_count == 0 || row->member_count > XR_BUILTIN_ENUM_MAX_MEMBERS)
            return false;
        char *members[XR_BUILTIN_ENUM_MAX_MEMBERS];
        int payloads[XR_BUILTIN_ENUM_MAX_MEMBERS];
        bool has_payload = false;
        for (uint32_t j = 0; j < row->member_count; j++) {
            members[j] = (char *) row->members[j].name;
            payloads[j] = row->members[j].has_payload ? 1 : 0;
            has_payload = has_payload || row->members[j].has_payload;
        }
        XrEnumType *type =
            xr_enum_type_new(isolate, "prelude", row->enum_name, members, (int) row->member_count);
        if (!type ||
            (has_payload &&
             !xr_enum_type_set_adt_payloads(type, payloads, (int) row->member_count)) ||
            (row->builtin_index == XR_GLOBAL_VAR_NUMBER_PARSE_ERROR &&
             !runtime_number_parse_error_is_exact(type)))
            return false;
        isolate_bind_builtin(isolate, row->builtin_index, XR_FROM_PTR(type));
    }
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    return true;
}
