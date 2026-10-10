/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_object_storage_prefix.c - Reject retired storage before owned headers
 *
 * KEY CONCEPT:
 *   A finite prefix grants no arena, reference or tail access.
 */
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "xir/xxir_class.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#error This finite-prefix test requires the qualified Windows provider
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PREFIX_HAS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define PREFIX_HAS_ASAN 1
#endif
#if defined(PREFIX_HAS_ASAN)
#include <sanitizer/asan_interface.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "PREFIX %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_value_compile_owner.h"
static void *prefix_unreadable;
static size_t prefix_old_arena_access, prefix_runtime_allocations;
static void prefix_check_arena(const XrXirTypeArena *arena) {
    if (prefix_unreadable && (const void *)arena == prefix_unreadable) ++prefix_old_arena_access;
    CHECK(!prefix_unreadable || (const void *)arena != prefix_unreadable);
}
static const XrXirTypes *prefix_types(const XrXirTypeArena *arena) {
    prefix_check_arena(arena); return xr_xir_compile_type_arena_types(arena);
}
static const XrXirStorageLayout *prefix_storage(const XrXirTypeArena *arena, XrXirType type) {
    prefix_check_arena(arena); return xr_xir_compile_type_arena_storage(arena, type);
}
static bool prefix_layout(const XrXirTypeArena *arena, XrXirType type, XrXirLayout *layout) {
    prefix_check_arena(arena); return xr_xir_compile_type_arena_layout(arena, type, layout);
}
static bool prefix_arena_retain(XrXirTypeArena *arena) {
    prefix_check_arena(arena); return xr_xir_compile_type_arena_retain(arena);
}
static void prefix_arena_drop(XrXirTypeArena *arena) {
    prefix_check_arena(arena); xr_xir_compile_type_arena_drop(arena);
}
static void *prefix_malloc(size_t bytes) { ++prefix_runtime_allocations; return xr_malloc(bytes); }
static void *prefix_calloc(size_t count, size_t bytes) {
    ++prefix_runtime_allocations; return xr_calloc(count, bytes);
}
static void *prefix_realloc(void *pointer, size_t bytes) {
    ++prefix_runtime_allocations; return xr_realloc(pointer, bytes);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_realloc")
#define xr_xir_compile_type_arena_types(arena) prefix_types(arena)
#define xr_xir_compile_type_arena_storage(arena, type) prefix_storage(arena, type)
#define xr_xir_compile_type_arena_layout(arena, type, layout) prefix_layout(arena, type, layout)
#define xr_xir_compile_type_arena_retain(arena) prefix_arena_retain(arena)
#define xr_xir_compile_type_arena_drop(arena) prefix_arena_drop(arena)
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#define xr_malloc(bytes) prefix_malloc(bytes)
#define xr_calloc(count, bytes) prefix_calloc(count, bytes)
#define xr_realloc(pointer, bytes) prefix_realloc(pointer, bytes)
#include "cell_owner_value_physical.h"
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#undef xr_xir_compile_type_arena_drop
#undef xr_xir_compile_type_arena_retain
#undef xr_xir_compile_type_arena_layout
#undef xr_xir_compile_type_arena_storage
#undef xr_xir_compile_type_arena_types

enum { PREFIX_CLASS_T = 256, PREFIX_CELL_T, PREFIX_UNIT_CELL_T, PREFIX_FN_T };
static XrXirTypeArena *prefix_arena_new(ValueCompileOwner *compiler) {
    XrXirNominalFieldIdentity fields[] = {{{"value", 5}, XR_XIR_FIELD_MUTABLE}, {{"text", 4}, 0}};
    XrXirNominalIdentity identity = {.module = {"root", 4}, .name = {"Counter", 7}, .exported = 1,
        .fields = fields, .field_count = 2, .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity};
    XrXirType field_types[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, field_types, 2}},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_UNIT},
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_NONE}};
    XrXirTypes types = {nodes, 4, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context, &types, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
typedef struct PrefixLease { bool active; uint32_t releases; } PrefixLease;
static void prefix_binding_release(void *context) {
    PrefixLease *lease = context; CHECK(lease->active && !lease->releases); ++lease->releases;
}
static XrXirValueStatus prefix_binding_admit(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    PrefixLease *lease = context;
    if (!work || !*work) return XR_XIR_VALUE_LIMIT;
    --*work;
    return lease && lease->active && !lease->releases && binding && binding->owner == lease &&
        binding->release == prefix_binding_release && binding->entry == 3 && !binding->capture_count &&
        !binding->captures && type == (XrXirType)PREFIX_FN_T ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void prefix_reject_apis(const XrXirValue *value, XrXirValueAdmission *admission) {
    XrXirValue output = {0}, replacement = {XR_XIR_I64, 0, 42};
    XrXirValuePlace place = {0};
    unsigned activation = 0, frame = 0;
    XrXirCellAuthority authority = {&activation, &frame, 1};
    XrXirValuePathStep step = {XR_XIR_PATH_FIELD, (XrXirType)value->type, 0};
    XrXirValuePath path = {&step, 1};
    CHECK(!xr_xir_value_valid(value) && !xr_xir_value_arena(value));
    CHECK(!xr_xir_value_argument(value, admission->arena, (XrXirType)value->type));
    CHECK(xr_xir_value_copy(value, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(value, (XrXirType)value->type, admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xr_xir_function_binding(value));
    CHECK(xr_xir_cell_read(value, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_write(value, &replacement, admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xr_xir_cell_in_domain(value, admission->domain));
    CHECK(!xr_xir_cell_unborrowed(value) && !xr_xir_cell_is_projection(value));
    CHECK(!xr_xir_cell_canonical_root(value) && !xr_xir_cell_module_storage(value));
    CHECK(!xr_xir_cell_same_owner(value, value));
    CHECK(xr_xir_cell_loan_prepare(value, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_publication_prepare(value, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xr_xir_cell_publication_matches(value, NULL));
    CHECK(xr_xir_cell_authorized_read(value, NULL, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_write(value, NULL, &replacement, admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_place(value, NULL, admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_value_place(value, admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_project_authorized((XrXirType)PREFIX_CELL_T, value, &path,
        &authority, admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_get(value, 0, admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(value, 0, &replacement, admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!output.type && !output.reserved && !output.payload && !place.payload && !place.type);
}
static void prefix_fake_case(uint32_t kind, XrXirType type, XrXirValueAdmission *admission) {
    enum { PREFIX_BYTES = offsetof(XirObject, kind) + sizeof(uint32_t) };
    _Static_assert(PREFIX_BYTES < sizeof(XirObject) && PREFIX_BYTES % _Alignof(XirObject) == 0,
        "kind finishes the measured aligned finite prefix");
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    CHECK(page >= PREFIX_BYTES && page <= SIZE_MAX / 2);
    unsigned char *memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory); DWORD previous = 0;
    CHECK(VirtualProtect(memory + page, page, PAGE_NOACCESS, &previous));
    unsigned char *prefix = memory + page - PREFIX_BYTES;
    memset(prefix, 0, PREFIX_BYTES);
    _Atomic(uint32_t) *references = (_Atomic(uint32_t) *)(prefix + offsetof(XirObject, references));
    atomic_init(references, 1);
    XrXirDomain *domain = (XrXirDomain *)(memory + page);
    XrXirTypeArena *arena = (XrXirTypeArena *)(memory + page);
    memcpy(prefix + offsetof(XirObject, domain), &domain, sizeof(domain));
    memcpy(prefix + offsetof(XirObject, arena), &arena, sizeof(arena));
    memcpy(prefix + offsetof(XirObject, type), &type, sizeof(type));
    memcpy(prefix + offsetof(XirObject, kind), &kind, sizeof(kind));
    unsigned char before[PREFIX_BYTES]; memcpy(before, prefix, PREFIX_BYTES);
    XrXirValue value = {(uint32_t)type, 0, 0}; void *pointer = prefix;
    memcpy(&value.payload, &pointer, sizeof(pointer)); prefix_unreadable = memory + page;
    size_t runtime = prefix_runtime_allocations, live = cell_owner_live, bytes = cell_owner_bytes;
    size_t compiler_calls = value_compile_calls;
#if defined(PREFIX_HAS_ASAN)
    /* Keep the type/kind granule readable; poison references, domain and arena. */
    _Static_assert(offsetof(XirObject, type) % 8 == 0, "owned prefix ends on a shadow granule");
    __asan_poison_memory_region(prefix, offsetof(XirObject, type));
#endif
    prefix_reject_apis(&value, admission);
#if defined(PREFIX_HAS_ASAN)
    __asan_unpoison_memory_region(prefix, PREFIX_BYTES);
#endif
    CHECK(!prefix_old_arena_access && prefix_runtime_allocations == runtime);
    CHECK(cell_owner_live == live && cell_owner_bytes == bytes && value_compile_calls == compiler_calls);
    CHECK(atomic_load(references) == 1 && !memcmp(before, prefix, PREFIX_BYTES));
    prefix_unreadable = NULL; memset(&value, 0, sizeof(value));
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
}
static void prefix_copy_reset(const XrXirValue *value) {
    XrXirValue alias = {0}; XirObject *object = object_pointer(value);
    uint32_t references = atomic_load(&object->references);
    CHECK(xr_xir_value_copy(value, &alias) == XR_XIR_VALUE_OK && alias.type == value->type &&
        alias.payload == value->payload && atomic_load(&object->references) == references + 1);
    xr_xir_value_drop(&alias);
    CHECK(!alias.type && !alias.reserved && !alias.payload && atomic_load(&object->references) == references);
}
static void prefix_current_factories(XrXirTypeArena *arena, XrXirDomain *domain, bool close) {
    PrefixLease lease = {true, 0};
    XrXirValueAdmission admission = {arena, domain, prefix_binding_admit, &lease, 65536, 65536};
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    size_t live = cell_owner_live, bytes = cell_owner_bytes;
    XrXirValue text = {0}, object = {0}, cell = {0}, unit_cell = {0}, function = {0}, projection = {0}, output = {0};
    XrXirValue forty = {XR_XIR_I64, 0, 40}, unit = {0};
    CHECK(xr_xir_string_new(domain, "kept", 4, &text) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {forty, text};
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T, fields, 2, &admission, &object) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)PREFIX_CELL_T, &forty, &admission, &cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)PREFIX_UNIT_CELL_T, &unit, &admission, &unit_cell) == XR_XIR_VALUE_OK);
    XrXirFunctionBinding binding = {&lease, prefix_binding_release, 3, NULL, 0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType)PREFIX_FN_T, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&object)->kind == XIR_OBJECT_CLASS && object_pointer(&cell)->kind == XIR_OBJECT_CELL);
    CHECK(object_pointer(&unit_cell)->kind == XIR_OBJECT_CELL && object_pointer(&function)->kind == XIR_OBJECT_FUNCTION);
    CHECK(!((XirClassObject *)object_pointer(&object))->borrow_top);
    XirCell *physical = (XirCell *)object_pointer(&cell);
    CHECK(physical->initialized && !physical->projection_count && !physical->borrow_top &&
        !physical->module_storage && physical->allocation_bytes == sizeof(*physical));
    unsigned activation = 0, frame = 0;
    /* These primitives model checked driver authority, never a Program grant. */
    XrXirCellAuthority authority = {&activation, &frame, 1};
    XrXirValuePathStep step = {XR_XIR_PATH_FIELD, (XrXirType)PREFIX_CLASS_T, 0};
    XrXirValuePath path = {&step, 1};
    CHECK(xr_xir_cell_project_authorized((XrXirType)PREFIX_CELL_T, &object, &path,
        &authority, &admission, &projection) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&projection)->kind == XIR_OBJECT_CELL && xr_xir_cell_is_projection(&projection));
    prefix_copy_reset(&object); prefix_copy_reset(&cell); prefix_copy_reset(&unit_cell);
    prefix_copy_reset(&function); prefix_copy_reset(&projection);
    XrXirCellLoan loan = {0};
    CHECK(xr_xir_cell_loan_prepare(&projection, &authority) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection, &loan, &authority);
    CHECK(((XirClassObject *)object_pointer(&object))->borrow_top == &loan);
    xr_xir_cell_loan_release(&loan);
    XrXirCellLoan empty = {0};
    CHECK(!memcmp(&loan, &empty, sizeof(loan)) && !((XirClassObject *)object_pointer(&object))->borrow_top);
    if (close) xr_xir_domain_close(domain);
    CHECK(xr_xir_class_get(&object, 0, &admission, &output) == XR_XIR_VALUE_OK && output.payload == 40);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_cell_read(&cell, &output) == XR_XIR_VALUE_OK && output.payload == 40);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_cell_read(&unit_cell, &output) == XR_XIR_VALUE_OK && !output.type);
    CHECK(xr_xir_function_binding(&function)->owner == &lease);
    xr_xir_value_drop(&projection); xr_xir_value_drop(&function); xr_xir_value_drop(&unit_cell);
    xr_xir_value_drop(&cell); xr_xir_value_drop(&object); xr_xir_value_drop(&text);
    CHECK(!projection.type && !function.type && !unit_cell.type && !cell.type && !object.type && !text.type);
    CHECK(lease.releases == 1 && xr_xir_domain_stats(domain).live_bytes == baseline);
    CHECK(cell_owner_live == live && cell_owner_bytes == bytes);
    lease.active = false;
}
int main(void) {
    _Static_assert(XIR_OBJECT_CLASS == 0x101u && XIR_OBJECT_CELL == 0x102u && XIR_OBJECT_FUNCTION == 0x103u,
        "Root-selected physical kinds required");
    /* Keep this existing observer's other entry points referenced without running them. */
    (void)value_compile_limits; (void)value_compile_boundaries; (void)value_compile_arena;
    ValueCompileOwner compiler = {0};
    CHECK(value_compile_owner_new(&compiler, (XrCompileResourceLimits){1048576, 1048576, 1048576}, 100) == XR_XIR_OK);
    XrXirTypeArena *arena = prefix_arena_new(&compiler);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    const uint32_t kinds[] = {0x100u, 3u, 1u, UINT32_MAX, 42u};
    const XrXirType types[] = {(XrXirType)PREFIX_CLASS_T, (XrXirType)PREFIX_CELL_T, (XrXirType)PREFIX_FN_T,
        (XrXirType)PREFIX_CLASS_T, (XrXirType)PREFIX_UNIT_CELL_T};
    for (uint32_t i = 0; i < 5; ++i) prefix_fake_case(kinds[i], types[i], &admission);
    prefix_current_factories(arena, domain, false);
    prefix_current_factories(arena, domain, true);
    xr_xir_domain_drop(domain); xr_xir_compile_type_arena_drop(arena);
    CHECK(value_compile_stats(&compiler.context).live_bytes == compiler.baseline.live_bytes);
    value_compile_owner_release(&compiler);
    CHECK(!cell_owner_live && !cell_owner_bytes && !value_compile_live && !value_compile_bytes);
    puts("finite retired prefixes, fresh factories, exact loan reset and ordinary/closed owner release passed");
    return 0;
}
