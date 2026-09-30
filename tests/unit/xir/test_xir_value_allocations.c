/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_value_allocations.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */


#include "base/xmalloc.h"
#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t calls, live, fail_at = SIZE_MAX;

static void *counted_malloc(size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(size);
    if (pointer)
        ++live;
    return pointer;
}

static void *counted_calloc(size_t count, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        ++live;
    return pointer;
}

static void counted_free(void *pointer) {
    if (pointer) {
        CHECK(live > 0);
        --live;
    }
    xr_free(pointer);
}

static void *counted_realloc(void *pointer, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    bool was_null = pointer == NULL;
    void *replacement = xr_realloc(pointer, size);
    if (replacement && was_null)
        ++live;
    return replacement;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(pointer) counted_free(pointer)
#define xr_realloc(pointer, size) counted_realloc(pointer, size)


#include "xir/xxir_types.c"
#include "xir/xxir_constraints.c"
#include "xir/xxir_constraint_proof.c"
#include "xir/xxir_implementation.c"
#include "xir/xxir_implementation_verify.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_type_arena.c"
#include "xir/xxir_value.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_float.c"
#include "xir/xxir_call.c"
#include "xir/xxir_output.c"
static bool counted_float_sink(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    size_t *published = context;
    CHECK(stream == XR_XIR_STDOUT && length == 9 && !memcmp(bytes, "0.1 -0.0\n", 9));
    ++*published;
    return true;
}
static void float_output_allocation(void) {
    XrXirValue values[] = {{XR_XIR_F32, 0, INT64_C(0x3dcccccd)}, {XR_XIR_F64, 0, INT64_MIN}};
    XrXirOutputGroup group = {XR_XIR_STDOUT, values, 2, true};
    size_t published = 0, before = calls;
    XrXirOutputSink sink = {counted_float_sink, &published, 9};
    fail_at = calls;
    CHECK(!xr_xir_output_render(&sink, &group) && !published && !live);
    CHECK(calls == before + 1);
    fail_at = SIZE_MAX; before = calls;
    CHECK(xr_xir_output_render(&sink, &group) && published == 1 && !live);
    CHECK(calls == before + 1);
    before = calls; sink.byte_limit = 8;
    CHECK(!xr_xir_output_render(&sink, &group) && published == 1 && calls == before && !live);
}
static bool counted_sink(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    size_t *published = context;
    CHECK(stream == XR_XIR_STDOUT && length == 12 && !memcmp(bytes, "false 0 123\n", 12));
    ++*published;
    return true;
}
static void output_allocation(void) {
    XrXirValue values[] = {{XR_XIR_BOOL, 0, 0}, {XR_XIR_I64, 0, 0}, {XR_XIR_I64, 0, 123}};
    XrXirOutputGroup group = {XR_XIR_STDOUT, values, 3, true};
    size_t published = 0;
    XrXirOutputSink sink = {counted_sink, &published, 12};
    fail_at = calls;
    CHECK(!xr_xir_output_render(&sink, &group) && !published && !live);
    fail_at = SIZE_MAX;
    CHECK(xr_xir_output_render(&sink, &group) && published == 1 && !live);
    size_t before = calls;
    sink.byte_limit = 10;
    CHECK(!xr_xir_output_render(&sink, &group));
    sink.byte_limit = 12;
    values[0].payload = 2;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[0].payload = 0; values[1].reserved = 1;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[1].reserved = 0; values[1].type = XR_XIR_UNIT;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[1].type = XR_XIR_I64;
    group.stream = XR_XIR_STDERR;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.stream = XR_XIR_STDOUT; group.line = false;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.line = true; group.count = 65537;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.count = 3; group.values = NULL;
    CHECK(!xr_xir_output_render(&sink, &group));
    CHECK(calls == before && published == 1 && !live);
}
static XrXirAction identity_resume(XrXirCallView *view) {
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->arguments[0], {0}, 0};
}
static void fail_sequence(void) {
    XrXirDomain *domain = NULL;
    XrXirValue left = {0}, right = {0}, copy = {0};
    XrXirValueStatus status = xr_xir_domain_new(65536, &domain);
    if (status != XR_XIR_VALUE_OK) goto done;
    status = xr_xir_string_new(domain, "123456789012345", 15, &left);
    if (status != XR_XIR_VALUE_OK) goto done;
    status = xr_xir_string_new(domain, "123456789012345", 15, &right);
    if (status != XR_XIR_VALUE_OK) goto done;
    size_t baseline = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    status = xr_xir_string_append(&left, &right);
    if (status != XR_XIR_VALUE_OK) {
        CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
        CHECK(string_pointer(&left)->length == 15);
        goto done;
    }
    CHECK(xr_xir_value_copy(&left, &copy) == XR_XIR_VALUE_OK);
    baseline = live; bytes = xr_xir_domain_stats(domain).live_bytes;
    status = xr_xir_string_append(&left, &right);
    if (status != XR_XIR_VALUE_OK) {
        CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
        CHECK(string_pointer(&left)->length == 30 && left.payload == copy.payload);
        goto done;
    }
    XrXirType type = XR_XIR_STRING;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_STRING, 0, identity_resume, NULL, NULL, 0, 0};
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting, {NULL, NULL}, {0}};
    XrXirCall *call = NULL;
    XrXirCallStatus admitted = xr_xir_call_new(&config, 0, &left, 1, &call);
    if (admitted == XR_XIR_CALL_READY) {
        xr_xir_value_drop(&left);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_take_result(call, &left) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    } else CHECK(admitted == XR_XIR_CALL_OOM && !call);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
 done:
    CHECK(status == XR_XIR_VALUE_OK || status == XR_XIR_VALUE_OOM);
    xr_xir_domain_drop(domain);
    xr_xir_value_drop(&left); xr_xir_value_drop(&right); xr_xir_value_drop(&copy);
    CHECK(!live);
}
static void saturation(void) {
    XrXirDomain *domain = NULL; XrXirValue value = {0}, copy = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "x", 1, &value) == XR_XIR_VALUE_OK);
    XirString *string = string_pointer(&value);
    atomic_store(&string->object.references, UINT32_MAX);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    XrXirValue first = {0};
    CHECK(xr_xir_string_new(domain, "first", 5, &first) == XR_XIR_VALUE_OK);
    XrXirValue arguments[] = {first, value};
    XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING};
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, parameters, 2, XR_XIR_STRING,
        0, identity_resume, NULL, NULL, 0, 0};
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting, {NULL, NULL}, {0}};
    XrXirCall *call = NULL;
    size_t baseline = live;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_LIMIT && !call);
    CHECK(live == baseline && !accounting.live_bytes && accounting.allocations == accounting.frees);
    CHECK(atomic_load(&string_pointer(&first)->object.references) == 1);
    xr_xir_value_drop(&first);
    atomic_store(&string->object.references, 1);
    atomic_store(&domain->references, UINT32_MAX);
    CHECK(xr_xir_string_new(domain, "y", 1, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    atomic_store(&domain->references, 2);
    xr_xir_value_drop(&value); xr_xir_domain_drop(domain);
    CHECK(!live);
}
static void capture_release(void *owner) { ++*(size_t *) owner; }
static XrXirValueStatus capture_admit(void *context, const XrXirFunctionBinding *binding,
                                     XrXirType type, uint64_t *work) {
    uint64_t cost = (uint64_t) binding->capture_count + 1;
    if (*work < cost) return XR_XIR_VALUE_LIMIT;
    *work -= cost;
    return type == (XrXirType) 256 && binding->owner == context && binding->release == capture_release &&
        binding->entry <= 7 ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirTypeArena *allocation_arena(XrXirDomain *domain) {
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType) 256},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 260},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 256},
    };
    XrXirTypes types = {nodes, 7, NULL, NULL};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.metadata_bytes = 65536; budget.work = 65536;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValueAdmission allocation_admission(XrXirDomain *domain, XrXirTypeArena *arena,
                                               size_t *owner) {
    return (XrXirValueAdmission) {arena, domain, capture_admit, owner, UINT64_MAX, 0};
}
static void capture_ownership(void) {
    XrXirDomain *domain = NULL; XrXirValue values[2] = {{0}}, output = {0};
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"first",5,&values[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"second",6,&values[1]) == XR_XIR_VALUE_OK);
    size_t releases = 0;
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    size_t baseline = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    XrXirFunctionBinding binding = {&releases,capture_release,7,values,2};
    atomic_store(&object_pointer(values+1)->references,UINT32_MAX);
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&output) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!output.type && !releases && live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    CHECK(atomic_load(&object_pointer(values)->references) == 1);
    atomic_store(&object_pointer(values+1)->references,1);
    fail_at = calls;
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&output) == XR_XIR_VALUE_OOM);
    CHECK(!output.type && !releases && live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    fail_at = SIZE_MAX;
    domain->limit = bytes;
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&output) == XR_XIR_VALUE_LIMIT);
    domain->limit = 65536;
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&output) == XR_XIR_VALUE_OK);
    XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&output,&copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(values,values+1) == XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *owned = xr_xir_function_binding(&output);
    const char *text; size_t length;
    CHECK(owned->captures != values && owned->capture_count == 2);
    CHECK(xr_xir_string_view(owned->captures,&text,&length) && length == 5 && !memcmp(text,"first",5));
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_value_drop(values); xr_xir_value_drop(values+1);
    xr_xir_value_drop(&output); CHECK(!releases);
    xr_xir_value_drop(&copy); CHECK(releases == 1 && !live);
}
static void deep_capture_release(void) {
    XrXirDomain *domain = NULL; XrXirValue previous = {XR_XIR_I64,0,17};
    CHECK(xr_xir_domain_new(32u*1024u*1024u,&domain) == XR_XIR_VALUE_OK);
    size_t releases = 0;
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    for (uint32_t depth = 0; depth < 100000; ++depth) {
        XrXirValue next = {0};
        XrXirFunctionBinding binding = {&releases,capture_release,0,&previous,1};
        CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&next) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&previous); previous = next;
    }
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    size_t allocations = calls;
    fail_at = calls;
    xr_xir_value_drop(&previous);
    CHECK(releases == 100000 && !live && calls == allocations);
    fail_at = SIZE_MAX;
    puts("Capture cleanup: 100000 nested environments; zero cleanup allocations; zero live blocks");
}
static void arena_allocation_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    size_t baseline = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    XrXirCallableParameter parameter = {XR_XIR_STRING, 0};
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_CALLABLE, .parameters = &parameter,
        .parameter_count = 1, .result = XR_XIR_I64};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 10; budget.metadata_bytes = 65536; budget.work = 100;
    XrXirBudget before = budget;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    uint64_t storage_bytes = before.metadata_bytes - budget.metadata_bytes;
    uint64_t storage_work = before.work - budget.work;
    CHECK(xr_xir_domain_stats(domain).live_bytes == bytes + storage_bytes);
    xr_xir_type_arena_drop(arena); arena = NULL; budget = before;
    fail_at = calls;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OOM && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    fail_at = SIZE_MAX;
    atomic_store(&domain->references, UINT32_MAX);
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_REFCOUNT_LIMIT && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    atomic_store(&domain->references, 1);
    budget.work = 0;
    before = budget;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    budget.work = 100;
    budget.metadata_bytes = storage_bytes - 1;
    before = budget;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    budget.metadata_bytes++;
    before = budget;
    domain->limit = bytes + budget.metadata_bytes - 1;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    domain->limit = 65536;
    parameter.type = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_BAD_ARGUMENT && !arena);
    CHECK(!memcmp(&budget, &before, sizeof(budget)));
    parameter.type = XR_XIR_STRING; node.parameter_span = 0;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_stats(domain).live_bytes == bytes + before.metadata_bytes);
    CHECK(budget.metadata_bytes == 0 && budget.work == 100 - storage_work && budget.parameters == 9);
    size_t releases = 0;
    XrXirFunctionBinding binding = {&releases, capture_release, 0, NULL, 0};
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    XrXirValue function = {0};
    atomic_store(&arena->references, UINT32_MAX);
    CHECK(!xr_xir_type_arena_retain(arena));
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &function) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!function.type && !releases && atomic_load(&domain->references) == 2);
    atomic_store(&arena->references, 1);
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    size_t allocations = calls; fail_at = calls;
    CHECK(xr_xir_value_valid(&function));
    xr_xir_value_drop(&function);
    CHECK(!live && releases == 1 && calls == allocations);
    fail_at = SIZE_MAX;
}
static void deep_arena_release(void) {
    const uint32_t count = 2048;
    XrXirTypeNode *nodes = counted_calloc(count, sizeof(*nodes));
    CHECK(nodes);
    for (uint32_t i = 0; i < count; ++i) {
        nodes[i].kind = XR_XIR_TYPE_ARRAY;
        nodes[i].element = i ? (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i - 1) : XR_XIR_STRING;
    }
    XrXirTypes types = {nodes, count, NULL, NULL};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.metadata_bytes = 1024 * 1024; budget.work = 8 * 1024 * 1024;
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1024 * 1024, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    counted_free(nodes);
    xr_xir_domain_drop(domain);
    size_t allocations = calls; fail_at = calls;
    xr_xir_type_arena_drop(arena);
    CHECK(!live && calls == allocations); fail_at = SIZE_MAX;
}
#include "xir_cell_allocation_cases.h"
#include "xir_array_allocation_cases.h"
#include "xir_nominal_fixture.h"
static void nominal_arena_allocation(void) {
    NominalIdentityFixture f; nominal_identity_fixture(&f);
    XrXirType field_types[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, NULL, 0, field_types, 2}};
    XrXirTypes types = {&node, 1, &f.table, NULL};
    calls = 0; fail_at = SIZE_MAX;
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    size_t baseline = live;
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 10000;
    XrXirTypeArena *arena = NULL;
    for (size_t i = 0; i < 3; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OOM);
        CHECK(!arena && live == baseline && budget.metadata_bytes == 65536 && budget.work == 10000);
    }
    calls = 0; fail_at = SIZE_MAX;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    CHECK(calls == 3 && live == baseline + 1);
    memset(&f, 0xCC, sizeof(f)); memset(field_types, 0xCC, sizeof(field_types)); memset(&node, 0xCC, sizeof(node));
    CHECK(xr_xir_type_arena_types(arena)->nodes[0].nominal.fields[0] == XR_XIR_I64);
    CHECK(xr_xir_type_arena_types(arena)->nodes[0].nominal.fields[1] == XR_XIR_STRING);
    xr_xir_domain_drop(domain);
    CHECK(!memcmp(xr_xir_type_arena_types(arena)->nominals->identities[0].name.bytes, "Pair", 4));
    size_t allocations = calls; fail_at = calls;
    xr_xir_type_arena_drop(arena);
    CHECK(live == 0 && calls == allocations);
    fail_at = SIZE_MAX; calls = 0;
}

#include "xir_struct_value_cases.h"
#include "xir_class_value_cases.h"
#include "xir_class_array_value_cases.h"
static void struct_allocation_failures(void) {
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = struct_value_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue fields[2] = {{XR_XIR_I64, 0, 7}, {0}}, leaf = {0}, parent = {0};
    CHECK(xr_xir_string_new(domain, "label", 5, &fields[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)256, fields, 2, &admission, &leaf) == XR_XIR_VALUE_OK);
    XrXirValue outer[] = {leaf, fields[1]};
    CHECK(xr_xir_struct_new((XrXirType)257, outer, 2, &admission, &parent) == XR_XIR_VALUE_OK);
    size_t baseline = live;
    for (unsigned mode = 0; mode < 3; ++mode) {
        size_t sites = 0;
        for (size_t attempt = 0; attempt <= sites; ++attempt) {
            XrXirValue destination = {0}, output = {0};
            CHECK(xr_xir_value_copy(&parent, &destination) == XR_XIR_VALUE_OK);
            admission.work = 100000; calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
            XrXirValueStatus status = mode == 0 ? xr_xir_struct_new((XrXirType)257, outer, 2, &admission, &output) :
                mode == 1 ? xr_xir_struct_get(&parent, 0, &admission, &output) :
                xr_xir_struct_set(&(XrXirValuePlace) {(XrXirType) destination.type, &destination.payload}, 0, &leaf, &admission);
            if (!attempt) { CHECK(status == XR_XIR_VALUE_OK); sites = calls; }
            else {
                CHECK(status == XR_XIR_VALUE_OOM && !output.type && destination.payload == parent.payload);
                CHECK(live == baseline && admission.scratch_bytes == 65536);
            }
            fail_at = SIZE_MAX;
            xr_xir_value_drop(&output); xr_xir_value_drop(&destination); CHECK(live == baseline);
        }
    }
    XrXirValue rejected = {0};
    XirObject *string = object_pointer(&fields[1]);
    uint32_t references = atomic_load(&string->references);
    atomic_store(&string->references, UINT32_MAX); admission.work = 100000;
    CHECK(xr_xir_struct_new((XrXirType)257, outer, 2, &admission, &rejected) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!rejected.type && live == baseline);
    atomic_store(&string->references, references);
    xr_xir_value_drop(&parent); xr_xir_value_drop(&leaf); xr_xir_value_drop(&fields[1]);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live);
    XrXirValue deep = struct_deep_value(); size_t before = calls; fail_at = calls;
    xr_xir_value_drop(&deep); CHECK(!live && calls == before); fail_at = SIZE_MAX;
}
static void string_predicate_allocations(void) {
    XrXirDomain *domain = NULL;
    XrXirValue text = {0}, pattern = {0}, copy = {0};
    CHECK(!live && fail_at == SIZE_MAX);
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "a\0abcdefghijklmnop", 18, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "abcdefghijklmnop", 16, &pattern) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&text, &copy) == XR_XIR_VALUE_OK);
    xr_xir_domain_drop(domain); xr_xir_value_drop(&text);
    size_t before = calls, allocations = live;
    uint32_t text_references = atomic_load(&object_pointer(&copy)->references);
    uint32_t pattern_references = atomic_load(&object_pointer(&pattern)->references);
    fail_at = calls;
    bool result = false;
    CHECK(xr_xir_string_contains(&copy, &pattern, &result) && result);
    CHECK(xr_xir_string_starts_with(&copy, &pattern, &result) && !result);
    CHECK(xr_xir_string_ends_with(&copy, &pattern, &result) && result);
    int64_t index = 99;
    CHECK(xr_xir_string_index_of(&copy, &pattern, 0, &index) == XR_XIR_VALUE_OK && index == 2);
    CHECK(xr_xir_string_last_index_of(&copy, &pattern, &index) == XR_XIR_VALUE_OK && index == 2);
    index = 99;
    CHECK(xr_xir_string_index_of(&copy, &pattern, INT64_MAX, &index) == XR_XIR_VALUE_BOUNDS && index == 99);
    XirString *stored = string_pointer(&copy);
    size_t saved_length = stored->length, saved_runes = stored->runes;
    stored->length = (size_t) PTRDIFF_MAX + 1;
    CHECK(xr_xir_string_index_of(&copy, &pattern, 0, &index) == XR_XIR_VALUE_LIMIT && index == 99);
    CHECK(xr_xir_string_last_index_of(&copy, &pattern, &index) == XR_XIR_VALUE_LIMIT && index == 99);
    stored->length = saved_length; stored->runes = (size_t) INT64_MAX + 1;
    CHECK(xr_xir_string_index_of(&copy, &pattern, 0, &index) == XR_XIR_VALUE_LIMIT && index == 99);
    CHECK(xr_xir_string_last_index_of(&copy, &pattern, &index) == XR_XIR_VALUE_LIMIT && index == 99);
    stored->runes = saved_runes;
    CHECK(calls == before && live == allocations);
    CHECK(atomic_load(&object_pointer(&copy)->references) == text_references);
    CHECK(atomic_load(&object_pointer(&pattern)->references) == pattern_references);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&pattern);
    CHECK(!live && calls == before);
    fail_at = SIZE_MAX;
}
#include "xir_enum_metadata_fixture.h"
static void enum_arena_allocations(void) {
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirBudget b = (XrXirBudget) {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000}; XrXirNominalTable *projection = NULL;
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_OK);
    size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX; b = (XrXirBudget) {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
        XrXirStatus status = xr_xir_nominal_project(&f.table, &b, &projection);
        if (!attempt) { CHECK(status == XR_XIR_OK); sites = calls; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !projection);
        xr_xir_nominal_free(projection); CHECK(!live);
    }
    fail_at = SIZE_MAX; b = (XrXirBudget) {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    CHECK(xr_xir_nominal_project(&f.table, &b, &projection) == XR_XIR_OK);
    XrXirTypes types = {NULL, 0, projection, NULL}; XrXirTypeArena *arena = NULL;
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    size_t baseline = live; b = (XrXirBudget) {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000}; calls = 0; fail_at = 0;
    CHECK(xr_xir_type_arena_new(domain, &types, &b, &arena) == XR_XIR_VALUE_OOM && !arena && live == baseline);
    fail_at = SIZE_MAX; b = (XrXirBudget) {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    CHECK(xr_xir_type_arena_new(domain, &types, &b, &arena) == XR_XIR_VALUE_OK);
    xr_xir_nominal_free(projection); memset(&f, 0xCC, sizeof(f)); xr_xir_domain_drop(domain);
    const XrXirNominalIdentity *id = xr_xir_type_arena_types(arena)->nominals->identities;
    CHECK(id->kind == XR_XIR_NOMINAL_ENUM && id->variant_count == 3);
    CHECK(id->variants[2].field_begin == 1 && !memcmp(id->variants[2].name.bytes, "Right", 5));
    xr_xir_type_arena_drop(arena); CHECK(!live); calls = 0;
}

static void enum_layout_allocation_failures(const XrXirTypes *types) {
    XrXirBudget initial = {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    XrXirBudget budget = initial; XrXirLayout layout = {0}; uint32_t offsets[2] = {99, 99};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    size_t begin = calls, baseline = live;
    CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OK);
    CHECK(layout.size == 24 && layout.alignment == 8 && offsets[0] == 8 && offsets[1] == 16);
    size_t count = calls - begin;
    for (size_t i = 0; i < count; ++i) {
        budget = initial; offsets[0] = offsets[1] = 99; fail_at = calls + i;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99);
        CHECK(live == baseline && !memcmp(&budget, &initial, sizeof(budget))); fail_at = SIZE_MAX;
    }
}
static void enum_arena_and_slot_boundaries(const XrXirTypes *types, const XrXirValue *empty,
    XrXirValueAdmission *admission) {
    size_t baseline = live;
    XrXirNominalIdentity identity = types->nominals->identities[0]; identity.module = (XrXirLiteral) {"beta", 4};
    XrXirNominalTable table = {NULL, 1, &identity}; XrXirTypes other = *types; other.nominals = &table;
    XrXirBudget budget = {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(admission->domain, &other, &budget, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission receiving = *admission; receiving.arena = arena;
    XrXirValue value = {0}, owned = {0};
    CHECK(xr_xir_enum_new((XrXirType)256, 0, NULL, 0, &receiving, &value) == XR_XIR_VALUE_OK);
    CHECK(!xr_xir_value_argument(empty, arena, (XrXirType)256));
    CHECK(xr_xir_value_admit(&value, (XrXirType)256, admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(empty, (XrXirType)256, &receiving) == XR_XIR_VALUE_BAD_ARGUMENT);
    int64_t slot = 0;
    CHECK(xr_xir_owned_slot_copy(&slot, 0, admission->arena, (XrXirType)256, empty->payload) == XR_XIR_VALUE_OK);
    int64_t previous = slot;
    CHECK(xr_xir_owned_slot_copy(&slot, 0, arena, (XrXirType)256, empty->payload) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(slot == previous);
    CHECK(xr_xir_value_copy(&value, &owned) == XR_XIR_VALUE_OK);
    xr_xir_owned_slot_move(&slot, 0, &owned); CHECK(!owned.type && !owned.payload);
    xr_xir_type_arena_drop(arena); xr_xir_value_drop(&value);
    XrXirValue borrowed = {(uint32_t)256, 0, slot};
    const XrXirNominalIdentity *escaped = &xr_xir_type_arena_types(xr_xir_value_arena(&borrowed))->nominals->identities[0];
    CHECK(escaped->module.length == 4 && !memcmp(escaped->module.bytes, "beta", 4));
    xr_xir_owned_slot_clear(&slot, 0); CHECK(!slot && live == baseline);
}
#include "xir_error_value_cases.h"
static void enum_value_ownership(void) {
    const XrXirNominalVariant variants[] = {{{"Empty", 5}, 0, 0}, {{"Pair", 4}, 0, 2}};
    const XrXirNominalFieldIdentity fields[] = {{{"left", 4}, 0}, {{"right", 5}, 0}};
    const XrXirNominalIdentity identity = {{"alpha", 5}, {"Choice", 6}, 1, 0, fields, 2,
        XR_XIR_NOMINAL_ENUM, variants, 2, 0};
    const XrXirNominalTable table = {NULL, 1, &identity};
    XrXirType field_types[] = {XR_XIR_STRING, XR_XIR_STRING};
    const XrXirType type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    node.nominal.fields = field_types; node.nominal.field_count = 2;
    const XrXirTypes types = {&node, 1, &table, NULL};
    XrXirBudget budget = {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(live == 0 && xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirBudget initial = budget;
    size_t arena_start = calls;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    size_t arena_calls = calls - arena_start;
    uint64_t required_metadata = initial.metadata_bytes - budget.metadata_bytes;
    uint64_t required_work = initial.work - budget.work;
    xr_xir_type_arena_drop(arena); arena = NULL; CHECK(live == 1);
    for (size_t i = 0; i < arena_calls; ++i) {
        budget = initial; fail_at = calls + i;
        CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OOM && !arena);
        CHECK(live == 1 && !memcmp(&budget, &initial, sizeof(budget))); fail_at = SIZE_MAX;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        budget = initial;
        if (i) budget.work = required_work - 1; else budget.metadata_bytes = required_metadata - 1;
        XrXirBudget saved = budget;
        CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT && !arena);
        CHECK(live == 1 && !memcmp(&budget, &saved, sizeof(budget)));
    }
    budget = initial;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {0}; admission.domain = domain; admission.arena = arena;
    admission.work = 100000; admission.scratch_bytes = 65536;
    XrXirValue empty = {0}, pair = {0}, values[2] = {{0}}, out = {0};
    size_t before = calls;
    fail_at = calls;
    CHECK(xr_xir_enum_new(type, 0, NULL, 0, &admission, &empty) == XR_XIR_VALUE_OK);
    for (uint32_t i = 0; i < 1000; ++i) {
        CHECK(xr_xir_value_copy(&empty, &out) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_admit(&out, type, &admission) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&out);
    }
    CHECK(calls == before); fail_at = SIZE_MAX;
    uint32_t leases = atomic_load(&arena->references);
    atomic_store(&arena->references, UINT32_MAX);
    CHECK(xr_xir_value_copy(&empty, &out) == XR_XIR_VALUE_REFCOUNT_LIMIT && !out.type);
    CHECK(xr_xir_enum_new(type, 0, NULL, 0, &admission, &out) == XR_XIR_VALUE_REFCOUNT_LIMIT && !out.type);
    atomic_store(&arena->references, leases);
    XirNominalValue impostor = {0};
    impostor.object = *object_pointer(&empty); impostor.variant = 0;
    XrXirValue forged = {type, 0, 0}; XirNominalValue *impostor_pointer = &impostor;
    memcpy(&forged.payload, &impostor_pointer, sizeof(impostor_pointer));
    CHECK(!xr_xir_value_valid(&forged));
    enum_layout_allocation_failures(&types);
    enum_arena_and_slot_boundaries(&types, &empty, &admission);
    uint32_t ordinal = 99;
    CHECK(xr_xir_enum_variant(&empty, &ordinal) == XR_XIR_VALUE_OK && ordinal == 0);
    CHECK(xr_xir_enum_get(&empty, 0, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    CHECK(xr_xir_struct_new(type, NULL, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_enum_new(type, 1, NULL, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_enum_new(type, 2, NULL, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_string_new(domain, "left", 4, &values[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "right", 5, &values[1]) == XR_XIR_VALUE_OK);
    size_t baseline = live; uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    fail_at = calls;
    CHECK(xr_xir_enum_new(type, 1, values, 2, &admission, &out) == XR_XIR_VALUE_OOM && !out.type);
    fail_at = SIZE_MAX;
    CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    XirObject *right = object_pointer(&values[1]);
    atomic_store(&right->references, UINT32_MAX);
    CHECK(xr_xir_enum_new(type, 1, values, 2, &admission, &out) == XR_XIR_VALUE_REFCOUNT_LIMIT && !out.type);
    CHECK(atomic_load(&object_pointer(&values[0])->references) == 1);
    atomic_store(&right->references, 1);
    CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    CHECK(xr_xir_enum_new(type, 1, values, 2, &admission, &pair) == XR_XIR_VALUE_OK);
    error_value_cases(&types, &empty, &pair, &admission);
    CHECK(xr_xir_enum_get(&pair, 0, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    CHECK(xr_xir_enum_get(&pair, 1, 2, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    CHECK(xr_xir_struct_get(&pair, 0, &admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    CHECK(xr_xir_string_append(&values[0], &values[1]) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&values[0]); xr_xir_value_drop(&values[1]);
    XrXirValue erased_empty = {0}, erased_pair = {0};
    CHECK(xr_xir_error_erase(&empty, &admission, &erased_empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&pair, &admission, &erased_pair) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&empty); xr_xir_value_drop(&pair);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(xr_xir_error_narrow(&erased_pair, type, &admission, &pair) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_narrow(&erased_empty, type, &admission, &empty) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&erased_pair); xr_xir_value_drop(&erased_empty);
    CHECK(xr_xir_enum_get(&pair, 1, 0, &admission, &out) == XR_XIR_VALUE_OK);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&out, &text, &length) && length == 4 && !memcmp(text, "left", 4));
    xr_xir_value_drop(&out); xr_xir_value_drop(&pair);
    CHECK(xr_xir_enum_variant(&empty, &ordinal) == XR_XIR_VALUE_OK && ordinal == 0);
    const XrXirNominalIdentity *escaped = &xr_xir_type_arena_types(xr_xir_value_arena(&empty))->nominals->identities[0];
    CHECK(escaped->module.length == 5 && !memcmp(escaped->module.bytes, "alpha", 5));
    xr_xir_value_drop(&empty); CHECK(live == 0);
}

static void deep_value_admission_failures(void) {
    fail_at = SIZE_MAX;
    XrXirValue value = struct_deep_value();
    XirObject *object = object_pointer(&value);
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 1000000, 65536};
    size_t baseline = live, begin = calls;
    uint64_t bytes = xr_xir_domain_stats(object->domain).live_bytes;
    CHECK(xr_xir_value_admit(&value, (XrXirType)value.type, &admission) == XR_XIR_VALUE_OK);
    size_t sites = calls - begin; CHECK(sites > 1);
    CHECK(admission.scratch_bytes == 65536 && live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        admission.work = 1000000; fail_at = calls + i;
        CHECK(xr_xir_value_admit(&value, (XrXirType)value.type, &admission) == XR_XIR_VALUE_OOM);
        fail_at = SIZE_MAX;
        CHECK(admission.scratch_bytes == 65536 && live == baseline);
        CHECK(xr_xir_domain_stats(object->domain).live_bytes == bytes && xr_xir_value_valid(&value));
    }
    admission.work = 80;
    CHECK(xr_xir_value_admit(&value, (XrXirType)value.type, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(admission.scratch_bytes == 65536 && live == baseline);
    admission.work = 1000000; admission.scratch_bytes = 100;
    CHECK(xr_xir_value_admit(&value, (XrXirType)value.type, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(admission.scratch_bytes == 100 && live == baseline);
    CHECK(xr_xir_domain_stats(object->domain).live_bytes == bytes);
    xr_xir_value_drop(&value); CHECK(!live);
}
#include "xir_storage_cache_cases.h"
#include "xir_storage_cursor_cases.h"
#include "xir_storage_pack_cases.h"
#include "xir_value_path_cases.h"
int main(void) {
    value_path_cases();
    storage_pack_cases();
    storage_cursor_cases();
    storage_cache_cases(); storage_cache_shared_graph();
    error_deep_value_cases();
    deep_value_admission_failures();
    enum_value_ownership();
    enum_arena_allocations();
    string_predicate_allocations();
    struct_allocation_failures();
    struct_value_cases(); CHECK(!live);
    class_array_field_value_cases();
    class_value_cases(); class_value_allocation_failures(); class_array_allocation_failures(); CHECK(!live);
    nominal_arena_allocation();
    fail_at = SIZE_MAX; calls = 0; fail_sequence();
    size_t count = calls;
    for (size_t i = 0; i < count; ++i) { fail_at = i; calls = 0; fail_sequence(); }
    fail_at = SIZE_MAX; saturation(); output_allocation(); float_output_allocation(); capture_ownership(); deep_capture_release();
    cell_allocation_cases(); cell_cycles_and_domains(); deep_cell_release();
    arena_allocation_cases(); deep_arena_release(); array_allocation_cases();
    printf("Managed allocation failures: %zu; every domain, string and activation physically released\n", count);
    return 0;
}
