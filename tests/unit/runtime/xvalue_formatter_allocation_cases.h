/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_formatter_allocation_cases.h - Real renderer failures without sticky buffers
 */
#ifndef XVALUE_FORMATTER_ALLOCATION_CASES_H
#define XVALUE_FORMATTER_ALLOCATION_CASES_H
#include "runtime/value/xvalue_format.h"
#include "runtime/class/xclass_builder.h"
#include "runtime/class/xinstance.h"
#include "runtime/object/xarray.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/core/xr_runtime_core.h"
#include "vm/xvm.h"
#include <stdlib.h>

#define FORMAT_ALLOC_REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "renderer requirement failed at %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)
/* Capture the real allocator entry before replacing only the included buffer
 * body's allocations. The rest of this test translation unit keeps using it. */
static inline void *fmt_system_malloc(size_t size) { return xr_malloc(size); }
static inline void *fmt_system_realloc(void *pointer, size_t size) { return xr_realloc(pointer, size); }
static inline void fmt_system_free(void *pointer) { xr_free(pointer); }
static size_t fmt_allocations, fmt_fail_at, fmt_live_blocks, fmt_intern_calls;
static bool fmt_reject_growth, fmt_reject_intern;
static XrValue fmt_method_result;
static void *fmt_malloc(size_t size) {
    if (++fmt_allocations == fmt_fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) ++fmt_live_blocks;
    return pointer;
}
static void fmt_free(void *pointer) {
    if (pointer) { FORMAT_ALLOC_REQUIRE(fmt_live_blocks != 0u); --fmt_live_blocks; }
    free(pointer);
}
static void *fmt_realloc(void *pointer, size_t size) {
    return fmt_reject_growth ? NULL : realloc(pointer, size);
}
static XrString *fmt_final_intern(XrVMRuntime *vm, const char *bytes, size_t size, uint32_t hash) {
    ++fmt_intern_calls;
    return fmt_reject_intern ? NULL : xr_string_intern(vm, bytes, size, hash);
}
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) fmt_malloc(size)
#define xr_free(pointer) fmt_free(pointer)
#define xr_realloc(pointer, size) fmt_realloc(pointer, size)
#define xr_strbuf_new fmt_buffer_new
#define xr_strbuf_free fmt_buffer_free
#define xr_strbuf_ensure fmt_buffer_ensure
#define xr_strbuf_reserve fmt_buffer_reserve
#define xr_strbuf_append_cstr fmt_buffer_append_cstr
#define xr_strbuf_append_str fmt_buffer_append_str
#define xr_strbuf_append_char fmt_buffer_append_char
#define xr_strbuf_append_int fmt_buffer_append_int
#define xr_strbuf_append_float fmt_buffer_append_float
#define xr_strbuf_reset fmt_buffer_reset
#include "../../../src/runtime/xstrbuf.c"
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) fmt_system_malloc(size)
#define xr_free(pointer) fmt_system_free(pointer)
#define xr_realloc(pointer, size) fmt_system_realloc(pointer, size)
#define xr_strbuf_to_string fmt_buffer_to_string
#define xr_string_intern fmt_final_intern
#include "../../../src/runtime/xstrbuf_string.c"
#define xr_value_to_strbuf fmt_value_to_strbuf
#define xr_value_to_string fmt_value_to_string
#define xr_value_to_string_checked fmt_value_to_string_checked
XR_FUNC bool fmt_value_to_strbuf(XrVMRuntime *, XrStrBuf *, XrValue, int, XrVMResult *);
XR_FUNC XrString *fmt_value_to_string(XrVMRuntime *, XrValue);
/* This is the single production renderer, including all recursive helpers.
 * No replacement FORMAT_NULL shortcut is installed. */
#include "../../../src/runtime/value/xvalue_format.c"
#undef xr_value_to_string_checked
#undef xr_value_to_string
#undef xr_value_to_strbuf
#undef xr_string_intern
#undef xr_strbuf_to_string
#undef xr_strbuf_reset
#undef xr_strbuf_append_float
#undef xr_strbuf_append_int
#undef xr_strbuf_append_char
#undef xr_strbuf_append_str
#undef xr_strbuf_append_cstr
#undef xr_strbuf_reserve
#undef xr_strbuf_ensure
#undef xr_strbuf_free
#undef xr_strbuf_new

static XrValue fmt_test_method(XrVMRuntime *vm, XrValue self, XrValue *args, int count) {
    (void) vm; (void) self; (void) args; (void) count;
    return fmt_method_result;
}
TEST(formatter_actual_allocation_failures) {
    for (unsigned mode = 0; mode < 8u; ++mode) {
        XrVMConfig config = {0};
        XrVMRuntime *vm = xray_vm_new_full(&config);
        FORMAT_ALLOC_REQUIRE(vm != NULL);
        xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
        char payload[512]; memset(payload, 'q', sizeof(payload));
        XrString *string = xr_string_intern(vm, payload, sizeof(payload), 0);
        FORMAT_ALLOC_REQUIRE(string != NULL);
        XrArray *array = xr_array_with_capacity_in(&vm->core_rt->root_alloc, 2, XR_ELEM_ANY);
        FORMAT_ALLOC_REQUIRE(array != NULL);
        xr_array_push(array, xr_string_value(string));
        XrValue input = xr_make_typed_ptr_val(array, XR_TARRAY);
        XrProto *method = NULL;
        if (mode >= 5u) {
            XrClassBuilder *builder = xr_class_builder_new(vm, "DateTime", NULL);
            FORMAT_ALLOC_REQUIRE(builder != NULL);
            fmt_method_result = mode == 6u ? xr_null() : xr_string_value(string);
            if (mode == 6u || mode == 7u) {
                method = xr_instruction_unit_new();
                FORMAT_ALLOC_REQUIRE(method != NULL);
                method->maxstacksize = mode == 7u ? xr_vm_current_ctx(vm)->stack_capacity + 128 : 2;
                method->numparams = method->min_params = 1;
                xr_instruction_unit_write(method, CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
                XrClosure *closure = xr_closure_new(vm, method, NULL);
                FORMAT_ALLOC_REQUIRE(closure != NULL);
                FORMAT_ALLOC_REQUIRE(xr_class_builder_add_method_closure(builder, "toString", closure,
                                        XMETHOD_CLOSURE, 0, 0, 0) == 0);
            } else {
                FORMAT_ALLOC_REQUIRE(xr_class_builder_add_method(builder, "toString", fmt_test_method, 0, 0) == 0);
            }
            XrClass *klass = xr_class_builder_finalize(builder);
            FORMAT_ALLOC_REQUIRE(klass != NULL);
            XrObjectInstance *instance = xr_instance_new(vm, klass);
            FORMAT_ALLOC_REQUIRE(instance != NULL);
            input = xr_make_typed_ptr_val(instance, XR_TINSTANCE);
        }
        fmt_allocations = fmt_intern_calls = 0u;
        fmt_fail_at = mode == 1u ? 1u : mode == 2u ? 2u : 0u;
        fmt_reject_growth = mode == 3u || mode == 5u;
        fmt_reject_intern = mode == 4u;
        XrString *result = fmt_value_to_string(vm, input);
        if (mode == 0u) {
            FORMAT_ALLOC_REQUIRE(result != NULL && result->length == sizeof(payload) + 4u);
            FORMAT_ALLOC_REQUIRE(memcmp(result->data, "[\"", 2u) == 0);
            FORMAT_ALLOC_REQUIRE(memcmp(result->data + 2u, payload, sizeof(payload)) == 0);
            FORMAT_ALLOC_REQUIRE(memcmp(result->data + 2u + sizeof(payload), "\"]", 2u) == 0);
        } else if (mode == 6u) {
            FORMAT_ALLOC_REQUIRE(result != NULL && result->length == 13u);
            FORMAT_ALLOC_REQUIRE(memcmp(result->data, "DateTime{...}", 13u) == 0);
        } else {
            FORMAT_ALLOC_REQUIRE(result == NULL);
        }
        FORMAT_ALLOC_REQUIRE(fmt_live_blocks == 0u);
        if (mode == 1u || mode == 2u || mode == 3u || mode == 5u || mode == 7u)
            FORMAT_ALLOC_REQUIRE(fmt_intern_calls == 0u);
        else
            FORMAT_ALLOC_REQUIRE(fmt_intern_calls == 1u);
        /* A failed recursive render has no global sticky state. The ordinary
         * buffer remains reusable after the caller chooses to discard a prefix. */
        fmt_reject_growth = false;
        fmt_reject_intern = false;
        fmt_fail_at = 0u;
        XrStrBuf *buffer = fmt_buffer_new(vm, 64u);
        FORMAT_ALLOC_REQUIRE(buffer != NULL);
        fmt_reject_growth = true;
        FORMAT_ALLOC_REQUIRE(!fmt_value_to_strbuf(vm, buffer, xr_string_value(string), 0, NULL));
        FORMAT_ALLOC_REQUIRE(buffer->length == 0u);
        fmt_reject_growth = false;
        FORMAT_ALLOC_REQUIRE(fmt_buffer_append_cstr(buffer, "ok", 2u));
        fmt_buffer_free(buffer);
        FORMAT_ALLOC_REQUIRE(fmt_live_blocks == 0u);
        xray_vm_delete(vm);
        xr_instruction_unit_free(method);
    }
}
#endif // XVALUE_FORMATTER_ALLOCATION_CASES_H
