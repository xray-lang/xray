/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_execution.c - Real leaf recovery and nested synchronous binding
 *
 * KEY CONCEPT:
 *   Controlled host callbacks test the common boundary, independently of VM
 *   dispatch. No standalone global is supplied to satisfy this executable.
 */
#include "runtime/native/xnative_execution.h"
#include "base/xmalloc.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_output.h"
#include "vm/register/xrbc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

#if defined(XR_NATIVE_STRING_ALLOCATIONS)
typedef struct NativeStringBlock { void *pointer; size_t bytes; } NativeStringBlock;
static NativeStringBlock native_string_blocks[64];
static size_t native_string_calls, native_string_live, native_string_bytes;
static size_t native_string_fail_at = SIZE_MAX;
static bool native_string_injected;
static bool native_string_fail(void) {
    if (native_string_calls++ != native_string_fail_at) return false;
    native_string_injected = true;
    return true;
}
static void native_string_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(native_string_live < 64 && bytes <= SIZE_MAX - native_string_bytes);
    native_string_blocks[native_string_live++] = (NativeStringBlock){pointer, bytes};
    native_string_bytes += bytes;
}
static size_t native_string_block(void *pointer) {
    size_t i = 0;
    while (i < native_string_live && native_string_blocks[i].pointer != pointer) ++i;
    CHECK(i < native_string_live);
    return i;
}
static void *native_string_malloc(size_t bytes) {
    if (native_string_fail()) return NULL;
    void *pointer = xr_malloc(bytes);
    native_string_record(pointer, bytes);
    return pointer;
}
static void *native_string_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX / count);
    if (native_string_fail()) return NULL;
    void *pointer = xr_calloc(count, bytes);
    native_string_record(pointer, count * bytes);
    return pointer;
}
static void native_string_free(void *pointer) {
    if (pointer) {
        size_t i = native_string_block(pointer);
        native_string_bytes -= native_string_blocks[i].bytes;
        native_string_blocks[i] = native_string_blocks[--native_string_live];
    }
    xr_free(pointer);
}
static void *native_string_realloc(void *pointer, size_t bytes) {
    CHECK(bytes);
    if (native_string_fail()) return NULL;
    size_t i = pointer ? native_string_block(pointer) : 0;
    void *replacement = xr_realloc(pointer, bytes);
    if (!replacement) return NULL;
    if (pointer) {
        native_string_bytes -= native_string_blocks[i].bytes;
        native_string_bytes += bytes;
        native_string_blocks[i] = (NativeStringBlock){replacement, bytes};
    } else native_string_record(replacement, bytes);
    return replacement;
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#pragma push_macro("xr_realloc")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(bytes) native_string_malloc(bytes)
#define xr_calloc(count, bytes) native_string_calloc(count, bytes)
#define xr_free(pointer) native_string_free(pointer)
#define xr_realloc(pointer, bytes) native_string_realloc(pointer, bytes)
#include "xir/xxir_value.c"
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#endif

typedef struct Context {
    XrtLocationOwner *owner;
    XrtExecutionFailure failure;
    unsigned visits, after;
    uint32_t panic_code, source_id;
    bool fail_inner;
    XrtExecutionStatus inner_status;
    struct Context *inner;
} Context;

static void normal_root(void *data) { ++((Context *)data)->visits; }
static void output_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_print_i64(17, 1);
    ++context->after;
}
static void panic_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_arithmetic_panic(context->panic_code, context->source_id);
}
static void string_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_print_string("embedded\0text", 13, 1);
    ++context->after;
}
static void nested_root(void *data) {
    Context *context = data;
    context->inner_status = xrt_run_embedded(context->inner->owner, &context->inner->failure,
        context->inner->panic_code ? panic_root : context->fail_inner ? output_root : normal_root,
        context->inner);
    if (context->panic_code) panic_root(context);
    else output_root(context);
}

static XrtLocationOwner *locations(const char *path) {
    XrtSourceLocation location = {path, strlen(path), 7, 9, 8, 11};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK);
    return owner;
}

#if defined(XR_OS_WINDOWS)
typedef HANDLE SavedOutput;
static SavedOutput disable_output(void) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
    return output;
}
static void restore_output(SavedOutput output) { CHECK(SetStdHandle(STD_OUTPUT_HANDLE, output)); }
#else
typedef int SavedOutput;
static SavedOutput disable_output(void) {
    int output = dup(STDOUT_FILENO);
    CHECK(output >= 0 && close(STDOUT_FILENO) == 0);
    return output;
}
static void restore_output(SavedOutput output) {
    CHECK(dup2(output, STDOUT_FILENO) >= 0 && close(output) == 0);
    clearerr(stdout);
}
#endif

static const char native_string_prefix[] = "Hello, \xE4\xB8\x96\xE7\x95\x8C\0v";
static const char native_string_expected[] = "Hello, \xE4\xB8\x96\xE7\x95\x8C\0v314159";

typedef struct NativeStringContext {
    XrtLocationOwner *owner;
    XrtExecutionFailure failure;
    XrXirDomain *source, *destination;
    XrXirValue left, right, joined, snapshot, moved;
    int64_t number;
    uint32_t panic_code;
    unsigned after;
} NativeStringContext;

static void native_string_equal(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && (!count || !memcmp(bytes, expected, count)));
}
static void native_string_root(void *data) {
    NativeStringContext *context = data;
    char buffer[XR_XIR_SCALAR_TEXT_BYTES]; const char *bytes = NULL; size_t length = 0;
    XrXirValue number = {XR_XIR_I64, 0, context->number};
    CHECK(xr_xir_scalar_text(&number, buffer, &bytes, &length));
    CHECK(xr_xir_string_new(context->source, native_string_prefix,
        sizeof(native_string_prefix) - 1, &context->left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(context->source, bytes, length, &context->right) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_concat(context->destination, &context->left, &context->right,
        &context->joined) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&context->joined, &context->snapshot) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_move(&context->joined, &context->moved) == XR_XIR_VALUE_OK);
    CHECK(!context->joined.type && !context->joined.payload);
    native_string_equal(&context->left, native_string_prefix, sizeof(native_string_prefix) - 1);
    if (context->panic_code) xrt_arithmetic_panic(context->panic_code, 1);
    CHECK(xr_xir_string_view(&context->moved, &bytes, &length));
    xrt_print_string(bytes, length, 1);
    ++context->after;
}
/* These handles live in the caller context, outside the root's setjmp frame. */
static void native_string_cleanup(NativeStringContext *context) {
    xr_xir_value_drop(&context->left); xr_xir_value_drop(&context->right);
    xr_xir_value_drop(&context->joined); xr_xir_value_drop(&context->snapshot);
    xr_xir_value_drop(&context->moved);
    xr_xir_domain_close(context->source); xr_xir_domain_drop(context->source);
    xr_xir_domain_close(context->destination); xr_xir_domain_drop(context->destination);
    xrt_locations_release(context->owner);
    xr_free(context);
}
static void native_string_recovery(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        NativeStringContext *context = xr_calloc(1, sizeof(*context));
        CHECK(context);
        context->owner = locations("native-string.xr");
        context->number = 314159;
        context->panic_code = mode >= 2 ? 418 + mode : 0;
        CHECK(xr_xir_domain_new(UINT64_MAX, &context->source) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_domain_new(UINT64_MAX, &context->destination) == XR_XIR_VALUE_OK);
        SavedOutput saved = mode == 1 ? disable_output() : (SavedOutput)0;
        XrtExecutionStatus status = xrt_run_embedded(context->owner, &context->failure,
            native_string_root, context);
        if (mode == 1) restore_output(saved);
        CHECK(status == (mode ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        CHECK(context->after == (mode ? 0u : 1u));
        if (mode == 1) {
            CHECK(context->failure.kind == XRT_FAILURE_OUTPUT);
            CHECK(context->failure.detail.output.io.failed && context->failure.detail.output.io.code);
            CHECK(context->failure.detail.output.source_id == 1);
        } else if (mode >= 2) {
            CHECK(context->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC);
            CHECK(context->failure.detail.panic.code == 418 + mode);
            CHECK(context->failure.detail.panic.source_id == 1);
        }
        native_string_equal(&context->snapshot, native_string_expected, sizeof(native_string_expected) - 1);
        native_string_equal(&context->moved, native_string_expected, sizeof(native_string_expected) - 1);
        XrXirValue escaped = {0};
        if (!mode) CHECK(xr_xir_value_move(&context->moved, &escaped) == XR_XIR_VALUE_OK);
        native_string_cleanup(context);
        if (!mode) {
            native_string_equal(&escaped, native_string_expected, sizeof(native_string_expected) - 1);
            xr_xir_value_drop(&escaped);
        }
    }
}
static XrXirValueStatus native_string_held(uint64_t limit, const XrXirValue *left,
    const XrXirValue *right, uint64_t *peak) {
    XrCompileResourceLimits limits = {limit ? XR_COMPILE_MEMORY_FINITE : XR_COMPILE_MEMORY_ORDINARY,
        limit, false};
    XrCompileResources *resources = NULL; XrXirDomain *destination = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats owner = {0}, baseline = {0}, after = {0};
    CHECK(xr_compile_resources_stats(resources, &owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_domain_new(resources, UINT64_MAX, &destination) == XR_XIR_VALUE_OK);
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        XrXirValue output = {0};
        status = xr_xir_string_concat(destination, left, right, &output);
        if (status == XR_XIR_VALUE_OK) native_string_equal(&output, "abcd", 4);
        else CHECK(status == XR_XIR_VALUE_MEMORY_LIMIT && !output.type && !output.payload);
        CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
        if (peak) *peak = after.peak_bytes;
        xr_xir_value_drop(&output);
        CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.held_bytes == baseline.held_bytes);
    }
    xr_xir_domain_close(destination); xr_xir_domain_drop(destination);
    CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.held_bytes == owner.held_bytes);
    xr_compile_resources_release(resources);
    return status;
}
static void native_string_concat_cases(void) {
    XrXirDomain *source = NULL, *destination = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX, &source) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(UINT64_MAX, &destination) == XR_XIR_VALUE_OK);
    XrXirValue left = {0}, right = {0}, output = {0}, empty = {0};
    CHECK(xr_xir_string_new(source, "ab", 2, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(source, "cd", 2, &right) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(source, NULL, 0, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_concat(source, &left, &left, &output) == XR_XIR_VALUE_OK);
    native_string_equal(&output, "abab", 4); native_string_equal(&left, "ab", 2);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_string_concat(source, &left, &empty, &output) == XR_XIR_VALUE_OK);
    CHECK(output.payload == left.payload);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_string_concat(destination, &empty, &right, &output) == XR_XIR_VALUE_OK);
    native_string_equal(&output, "cd", 2);
    xr_xir_value_drop(&output);
    int64_t prior = left.payload;
    CHECK(xr_xir_string_concat(destination, &left, &right, &left) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(left.payload == prior);
    CHECK(xr_xir_string_concat(NULL, &left, &right, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_string_concat(destination, NULL, &right, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!output.type && !output.payload);
    uint64_t peak = 0;
    CHECK(native_string_held(0, &left, &right, &peak) == XR_XIR_VALUE_OK && peak);
    CHECK(native_string_held(peak, &left, &right, NULL) == XR_XIR_VALUE_OK);
    CHECK(native_string_held(peak - 1, &left, &right, NULL) == XR_XIR_VALUE_MEMORY_LIMIT);
    fprintf(stderr, "Shared String current held peak: %llu\n", (unsigned long long)peak);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    XrXirDomainStats baseline = xr_xir_domain_stats(destination);
    for (size_t site = 0; site < 2; ++site) {
        size_t live = native_string_live, bytes = native_string_bytes;
        native_string_injected = false;
        native_string_fail_at = native_string_calls + site;
        CHECK(xr_xir_string_concat(destination, &left, &right, &output) == XR_XIR_VALUE_OOM);
        native_string_fail_at = SIZE_MAX;
        CHECK(native_string_injected && !output.type && !output.payload);
        CHECK(native_string_live == live && native_string_bytes == bytes);
        CHECK(xr_xir_domain_stats(destination).live_bytes == baseline.live_bytes);
        native_string_equal(&left, "ab", 2); native_string_equal(&right, "cd", 2);
        CHECK(xr_xir_string_concat(destination, &left, &right, &output) == XR_XIR_VALUE_OK);
        native_string_equal(&output, "abcd", 4); xr_xir_value_drop(&output);
        CHECK(native_string_live == live && native_string_bytes == bytes);
    }
#endif
    xr_xir_value_drop(&left); xr_xir_value_drop(&right); xr_xir_value_drop(&empty);
    xr_xir_domain_close(source); xr_xir_domain_drop(source);
    xr_xir_domain_close(destination); xr_xir_domain_drop(destination);
}
static void native_shared_strings(void) {
    native_string_concat_cases(); native_string_recovery();
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
#endif
}

typedef struct OwnedStringContext {
    XrtLocationOwner *locations;
    XrtStringRoot strings;
    XrtExecutionFailure failure;
    XrRbcOutcome *outcome;
    XrtString *borrowed_body;
    unsigned mode, after;
    uint64_t peak;
} OwnedStringContext;

static void owned_string_equal(const XrtString *string, const char *expected, size_t count) {
    size_t length = SIZE_MAX;
    const char *bytes = xrt_string_bytes(string, &length);
    CHECK(length == count && (!count || !memcmp(bytes, expected, count)));
}
static XrRbcOutcome *owned_string_outcome(XrtLocationOwner *locations, XrXirValueStatus expected) {
    uint32_t code[2];
    CHECK(xr_rbc_encode(&code[0], (XrRbcOperands){XR_RBC_STRING_LITERAL,0,0,0,0}));
    CHECK(xr_rbc_encode(&code[1], (XrRbcOperands){XR_RBC_RET,0,0,0,0}));
    const XrRbcRegisterKind kinds[] = {XR_RBC_STRING};
    const char bytes[] = "\xc3\xa9\0Z";
    const XrRbcLiteral literal = {expected==XR_XIR_VALUE_BAD_UTF8 ? "\xff" : bytes,
        expected==XR_XIR_VALUE_BAD_UTF8 ? 1 : sizeof(bytes)-1};
    const XrRbcFunction function = {.code=code,.code_count=2,.register_count=1,
        .register_kinds=kinds,.register_kind_count=1,.result_kind=XR_RBC_STRING};
    const XrRbcModule module = {.functions=&function,.function_count=1,.literals=&literal,.literal_count=1};
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==
        (expected==XR_XIR_VALUE_OK ? XR_RBC_OK : XR_RBC_VALUE_FAILURE));
    xr_rbc_execution_free(&execution);
    uint64_t bits = 19;
    CHECK(!xr_rbc_outcome_bits(outcome,&bits) && bits==19);
    if (expected!=XR_XIR_VALUE_OK) {
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        uint32_t source_id = UINT32_MAX;
        const XrtLocationOwner *borrowed = NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome,&status,&source_id,&borrowed) &&
            status==expected && source_id==0 && borrowed==locations);
        const XrtString *missing = NULL;
        CHECK(!xr_rbc_outcome_string(outcome,&missing) && !missing);
    }
    return outcome;
}
static void owned_string_inner(void *data) {
    OwnedStringContext *context = data;
    xr_rbc_outcome_free(&context->outcome);
}
static void owned_string_nested(void *data) {
    OwnedStringContext *context = data;
    const XrtString *borrowed = NULL;
    CHECK(xr_rbc_outcome_string(context->outcome,&borrowed));
    XrtString *local = xrt_string_retain(borrowed,1);
    CHECK(local==borrowed);
    CHECK(xrt_string_length(borrowed,1)==3);
    if (!context->mode) {
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded(context->locations,&failure,owned_string_inner,context)==XRT_EXECUTION_COMPLETED);
    } else {
        XrtString *host = NULL;
        CHECK(xr_rbc_outcome_take_string(context->outcome,&host) && host==local);
        CHECK(!xr_rbc_outcome_take_string(context->outcome,&host));
        xr_rbc_outcome_free(&context->outcome);
        xrt_string_owned_drop(&host);
        CHECK(!host);
    }
    owned_string_equal(local,"\xc3\xa9\0Z",4);
    xrt_string_drop(&local);
    CHECK(!local);
    ++context->after;
}
static void owned_string_root(void *data) {
    OwnedStringContext *context = data;
    XrtString *empty = xrt_string_new("",0,1);
    CHECK(xrt_string_length(empty,1)==0);
    xrt_string_drop(&empty);
    XrtString *left = xrt_string_new("\xc3\xa9\0",3,1);
    XrtString *right = xrt_string_i64(-71,1);
    XrtString *joined = xrt_string_concat(left,right,1);
    CHECK(xrt_string_length(left,1)==2 && xrt_string_length(right,1)==3 &&
        xrt_string_length(joined,1)==5);
    XrtString *copy = xrt_string_retain(joined,1), *moved = NULL;
    uint64_t before = xr_xir_domain_stats(context->strings.domain).live_bytes;
    xrt_string_move(&moved,&copy);
    CHECK(!copy && moved==joined && xr_xir_domain_stats(context->strings.domain).live_bytes==before);
    owned_string_equal(moved,"\xc3\xa9\0-71",6);
    CHECK(xrt_string_length(moved,1)==5 &&
        xr_xir_domain_stats(context->strings.domain).live_bytes==before);
    context->peak = before;
    if (context->mode==1) xrt_print_string("fail",4,1);
    if (context->mode==2 || context->mode==3) xrt_arithmetic_panic(418+context->mode,1);
    if (context->mode==4) (void)xrt_clock_utc_offset_at(INT64_MAX,1);
    if (context->mode==5) {
        context->borrowed_body = xrt_string_root_take(moved);
        moved = NULL;
    }
    xrt_string_drop(&moved);
    xrt_string_drop(&joined);
    xrt_string_drop(&right);
    xrt_string_drop(&left);
    ++context->after;
}
static void native_owned_strings(void) {
    OwnedStringContext *context = xr_calloc(1,sizeof(*context)); CHECK(context);
    context->locations = locations("native-owned-string.xr");
    for (unsigned mode=0; mode<2; ++mode) {
        context->mode=mode; context->outcome=owned_string_outcome(context->locations,XR_XIR_VALUE_OK);
        CHECK(xrt_run_embedded_owned(context->locations,&context->failure,owned_string_nested,context,
            &context->strings)==XRT_EXECUTION_COMPLETED);
        CHECK(!context->outcome && !context->strings.owners && !context->strings.domain);
    }
    for (unsigned mode=0; mode<=5; ++mode) {
        context->mode=mode; context->after=0;
        SavedOutput saved = mode==1 ? disable_output() : (SavedOutput)0;
        XrtExecutionStatus status = xrt_run_embedded_owned(context->locations,&context->failure,
            owned_string_root,context,&context->strings);
        if (mode==1) restore_output(saved);
        CHECK(status==(mode>=1 && mode<=4 ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        CHECK(!context->strings.domain && !context->strings.owners);
        CHECK(context->after==(mode==0 || mode==5 ? 1u : 0u));
        if (mode==1) CHECK(context->failure.kind==XRT_FAILURE_OUTPUT);
        if (mode==2 || mode==3) CHECK(context->failure.kind==XRT_FAILURE_ARITHMETIC_PANIC &&
            context->failure.detail.panic.code==418+mode);
        if (mode==4) CHECK(context->failure.kind==XRT_FAILURE_HOST_CLOCK);
        if (mode==5) {
            owned_string_equal(context->borrowed_body,"\xc3\xa9\0-71",6);
            xrt_string_owned_drop(&context->borrowed_body);
        }
    }
    context->mode=0; context->after=0; context->strings.byte_limit=context->peak-1;
    CHECK(xrt_run_embedded_owned(context->locations,&context->failure,owned_string_root,context,
        &context->strings)==XRT_EXECUTION_FAILURE);
    CHECK(context->failure.kind==XRT_FAILURE_VALUE && context->failure.detail.value.status==XR_XIR_VALUE_LIMIT &&
        context->failure.detail.value.source_id==1 && !context->after && !context->strings.domain && !context->strings.owners);
    context->strings.byte_limit=context->peak;
    CHECK(xrt_run_embedded_owned(context->locations,&context->failure,owned_string_root,context,
        &context->strings)==XRT_EXECUTION_COMPLETED);
    context->strings.byte_limit=0;
    context->outcome=owned_string_outcome(context->locations,XR_XIR_VALUE_BAD_UTF8);
    xr_rbc_outcome_free(&context->outcome);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
    for (size_t site=0; site<3; ++site) {
        native_string_fail_at=native_string_calls+site; native_string_injected=false; context->after=0;
        CHECK(xrt_run_embedded_owned(context->locations,&context->failure,owned_string_root,context,
            &context->strings)==XRT_EXECUTION_FAILURE);
        native_string_fail_at=SIZE_MAX;
        CHECK(native_string_injected && context->failure.kind==XRT_FAILURE_VALUE &&
            context->failure.detail.value.status==XR_XIR_VALUE_OOM && !context->after);
        CHECK(!native_string_live && !native_string_bytes && !context->strings.domain && !context->strings.owners);
        native_string_fail_at=native_string_calls+site; native_string_injected=false;
        context->outcome=owned_string_outcome(context->locations,XR_XIR_VALUE_OOM);
        native_string_fail_at=SIZE_MAX;
        CHECK(native_string_injected);
        xr_rbc_outcome_free(&context->outcome);
        CHECK(!native_string_live && !native_string_bytes);
    }
#endif
    xrt_locations_release(context->locations); xr_free(context);
}

int main(void) {
    native_shared_strings();
    native_owned_strings();
    Context *outer = xr_calloc(1, sizeof(*outer));
    Context *inner = xr_calloc(1, sizeof(*inner));
    CHECK(outer && inner);
    outer->owner = locations("outer-source.xr");
    inner->owner = locations("inner-source.xr");
    outer->inner = inner;
    memset(&outer->failure, 0xa5, sizeof(outer->failure));
    unsigned char prior[sizeof(outer->failure)];
    memcpy(prior, &outer->failure, sizeof(prior));
    CHECK(xrt_run_embedded(NULL, &outer->failure, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, NULL, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, NULL, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->visits == 1 && !memcmp(prior, &outer->failure, sizeof(prior)));
    for (unsigned failure = 0; failure < 2; ++failure) {
        outer->after = inner->after = 0;
        outer->fail_inner = failure != 0;
        memset(&inner->failure, 0xa5, sizeof(inner->failure));
        SavedOutput saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer);
        restore_output(saved);
        CHECK(status == XRT_EXECUTION_FAILURE && !outer->after && !inner->after);
        CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT);
        XrtOutputFailure *output = &outer->failure.detail.output;
        CHECK(output->io.failed && output->io.code && output->source_id == 1);
        CHECK(output->io.domain != XRT_IO_NONE);
        CHECK(outer->inner_status == (failure ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        if (failure) {
            CHECK(inner->failure.kind == XRT_FAILURE_OUTPUT);
            CHECK(inner->failure.detail.output.io.failed && inner->failure.detail.output.source_id == 1);
        } else CHECK(!memcmp(prior, &inner->failure, sizeof(prior)));
        // The prior failed invocation leaves no expired recovery target behind.
        CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    }
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, output_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->after == 1);
    outer->after = 0;
    SavedOutput string_output = disable_output();
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, string_root, outer) == XRT_EXECUTION_FAILURE);
    restore_output(string_output);
    CHECK(!outer->after && outer->failure.kind == XRT_FAILURE_OUTPUT);
    CHECK(outer->failure.detail.output.operation == XRT_OUTPUT_WRITE);
    CHECK(outer->failure.detail.output.source_id == 1 && outer->failure.detail.output.io.failed);
    CHECK(outer->failure.detail.output.io.code && outer->failure.detail.output.io.domain != XRT_IO_NONE);
    const XrtSourceLocation *string_location = xrt_locations_get(outer->owner, 1);
    CHECK(string_location && string_location->line == 7 && string_location->column == 9);
    memcpy(prior, &outer->failure, sizeof(prior));
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(!memcmp(prior, &outer->failure, sizeof(prior)));
    for (uint32_t code = 420; code <= 421; ++code) {
        for (uint32_t id = 0; id <= 1; ++id) {
            outer->panic_code = code;
            outer->source_id = id;
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, panic_root, outer) == XRT_EXECUTION_FAILURE);
            CHECK(outer->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC);
            CHECK(outer->failure.detail.panic.code == code && outer->failure.detail.panic.source_id == id);
            memcpy(prior, &outer->failure, sizeof(prior));
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
            CHECK(!memcmp(prior, &outer->failure, sizeof(prior)));
        }
    }
    inner->panic_code = 420;
    inner->source_id = 1;
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    CHECK(outer->inner_status == XRT_EXECUTION_FAILURE);
    CHECK(inner->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && inner->failure.detail.panic.code == 420);
    CHECK(inner->failure.detail.panic.source_id == 1 && outer->failure.detail.panic.code == 421);
    // A recovered panic restores the enclosing output binding and its variant.
    outer->panic_code = 0;
    SavedOutput saved = disable_output();
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    restore_output(saved);
    CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT && outer->failure.detail.output.source_id == 1);
    CHECK(inner->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && outer->inner_status == XRT_EXECUTION_FAILURE);
    xrt_locations_release(inner->owner);
    xrt_locations_release(outer->owner);
    xr_free(inner);
    xr_free(outer);
    fputs("Synchronous runtime scope passed\n", stderr);
    return 0;
}
