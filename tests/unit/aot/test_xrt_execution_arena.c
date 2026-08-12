#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_allocations;
static int g_frees;

static void *arena_test_malloc(size_t size) {
    g_allocations++;
    return malloc(size);
}

static void *arena_test_calloc(size_t count, size_t size) {
    g_allocations++;
    return calloc(count, size);
}

static void *arena_test_realloc(void *ptr, size_t size) {
    if (!ptr)
        g_allocations++;
    return realloc(ptr, size);
}

static void arena_test_free(void *ptr) {
    if (ptr)
        g_frees++;
    free(ptr);
}

#define XRT_MALLOC(sz) arena_test_malloc(sz)
#define XRT_CALLOC(n, sz) arena_test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) arena_test_realloc((p), (sz))
#define XRT_FREE(p) arena_test_free(p)
#define XRT_IMPL
#include "../../../src/aot/xrt_coll.h"
#include "../../../src/aot/xrt_mem.h"

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    (void) exc;
    abort();
}

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", message);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct ArenaTestBox {
    XrObjHeader hdr;
    XrValue child;
} ArenaTestBox;

static void arena_test_box_destroy(void *obj) {
    ArenaTestBox *box = (ArenaTestBox *) obj;
    xrt_release(box->child);
}

static void arena_test_box_promote(void *obj, uint8_t storage_mode) {
    ArenaTestBox *box = (ArenaTestBox *) obj;
    (void) xrt_value_set_storage(box->child, storage_mode);
}

int main(void) {
    xrt_arc_init();

    void *arena = xrt_execution_arena_new();
    CHECK(arena != NULL, "execution arena is created");
    void *previous = xrt_execution_arena_enter(arena);

    XrValue scalar = xrt_str_from_cstr("short-lived");
    CHECK(xrt_execution_arena_live_objects(arena) == 1,
          "normal allocation is registered with current execution");
    XrString *scalar_string = (XrString *) scalar.ptr;
    CHECK(scalar_string->header.object_kind == XR_RUNTIME_OBJECT_KIND_STRING &&
              scalar_string->header.layout_id == XR_RUNTIME_STRING_LAYOUT_INDEX &&
              scalar_string->header.domain_id ==
                  XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
          "hosted AOT string uses the canonical materialized header");
    CHECK(offsetof(XrString, data) == XR_RUNTIME_STRING_FIXED_PREFIX_SIZE,
          "hosted AOT string uses the canonical inline-tail offset");
    CHECK(xr_str_value_from_ptr(scalar.ptr).tag == XR_TAG_STR_ARC,
          "raw AOT frame reconstruction recognizes the canonical string header");
    CHECK(xr_runtime_string_object_validate(
              scalar_string,
              xrt_execution_node(scalar_string)->object_bytes) ==
              XR_RUNTIME_ABI_OK,
          "hosted AOT string satisfies the canonical object verifier");
    xrt_retain(scalar);
    CHECK(atomic_load_explicit(&scalar_string->header.rc,
                               memory_order_relaxed) == 2,
          "hosted AOT retain uses positive canonical RC");
    xrt_release(scalar);
    CHECK(atomic_load_explicit(&scalar_string->header.rc,
                               memory_order_relaxed) == 1,
          "hosted AOT release preserves the remaining owner");
    xrt_release(scalar);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "normal RC release unlinks and reclaims an acyclic object");

    XrValue published_string = xrt_str_from_cstr("published-string");
    XrString *published_string_object = (XrString *) published_string.ptr;
    size_t published_string_bytes =
        (size_t) xrt_execution_node(published_string_object)->object_bytes;
    published_string =
        xrt_value_set_storage(published_string, XR_OBJ_STORAGE_SHARED);
    CHECK(xrt_execution_arena_live_objects(arena) == 0 &&
              published_string_object->header.domain_id ==
                  XR_RUNTIME_STRING_DOMAIN_CONST_SHARED &&
              published_string_object->rune_length != UINT32_MAX &&
              published_string_object->hash != 0 &&
              (atomic_load_explicit(&published_string_object->traits,
                                    memory_order_relaxed) &
               XR_RUNTIME_STRING_TRAIT_LOCAL) == 0 &&
              xr_runtime_string_object_validate(
                  published_string_object, published_string_bytes) ==
                  XR_RUNTIME_ABI_OK,
          "hosted AOT publication freezes caches and validates shared policy");
    xrt_release(published_string);

    XrValue json = xrt_struct_object_new(0);
    CHECK(((XrObjHeader *) json.ptr)->type == XR_TINSTANCE,
          "JSON uses the canonical instance object kind");
    CHECK(xrt_aot_class_type_id((XrObjHeader *) json.ptr) == 0,
          "builtin JSON destructor metadata is not a local class id");
    xrt_release(json);

    XrValue tuple_child = xrt_str_from_cstr("tuple-owned");
    XrValue tuple = xrt_tuple_make_from_borrowed(1, &tuple_child);
    xrt_release(tuple_child);
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "tuple takes an owning reference to each reference-valued lane");
    xrt_release(tuple);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "tuple destruction releases its owned lanes and allocation");

    XrValue enum_child = xrt_str_from_cstr("enum-owned");
    XrAotEnumAggregate enum_value =
        xrt_enum_aggregate_make(73, 1, 1, "Result", "Value", &enum_child);
    XrValue enum_box = xrt_enum_aggregate_box(enum_value);
    CHECK(xrt_arc_value_has_header(enum_box),
          "dynamic enum box participates in the AOT ARC domain");
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "dynamic enum box and its payload are execution-owned objects");
    xrt_retain(enum_box);
    xrt_release(enum_box);
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "dynamic enum box retain/release preserves its remaining owner");
    XrAotEnumAggregate taken = xrt_enum_aggregate_take_from_boxed(enum_box);
    CHECK(xrt_execution_arena_live_objects(arena) == 1,
          "consuming unbox reclaims the wrapper and transfers its payload owner");
    CHECK(strcmp(xr_str_data(taken.payloads[0]), "enum-owned") == 0,
          "consuming unbox keeps the transferred payload live");
    xrt_release(taken.payloads[0]);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "transferred enum payload follows ordinary inline aggregate cleanup");

    enum_child = xrt_str_from_cstr("enum-inline-arc");
    enum_value = xrt_enum_aggregate_make(73, 1, 1, "Result", "Value", &enum_child);
    xrt_enum_aggregate_retain(enum_value);
    xrt_enum_aggregate_release(enum_value);
    CHECK(xrt_execution_arena_live_objects(arena) == 1,
          "inline enum retain/release preserves its original payload owner");
    xrt_enum_aggregate_release(enum_value);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "inline enum release drops every active payload owner");

    enum_child = xrt_str_from_cstr("enum-borrowed");
    enum_value = xrt_enum_aggregate_make(73, 1, 1, "Result", "Value", &enum_child);
    enum_box = xrt_enum_aggregate_box_from_borrowed(enum_value);
    xrt_release(enum_child);
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "borrowed boxing acquires an independent payload owner");
    xrt_release(enum_box);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "enum box destruction releases every owned payload lane");

    XrValue left = xrt_cell_new(XR_NULL_VAL);
    XrValue right = xrt_cell_new(XR_NULL_VAL);
    xrt_retain(right);
    xrt_cell_access_set(left, right);
    xrt_retain(left);
    xrt_cell_access_set(right, left);
    xrt_release(left);
    xrt_release(right);
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "a strong cycle remains live under pure RC before execution teardown");

    xrt_execution_arena_restore(previous);
    int frees_before_cycle_sweep = g_frees;
    xrt_execution_arena_destroy(arena);
    CHECK(g_frees >= frees_before_cycle_sweep + 3,
          "execution teardown frees both cycle members and the arena owner");

    arena = xrt_execution_arena_new();
    CHECK(arena != NULL, "second execution arena is created");
    previous = xrt_execution_arena_enter(arena);
    XrValue published = xrt_map_new(1);
    XrValue published_child = xrt_cell_new(XR_NULL_VAL);
    xrt_map_set((xrt_map_t *) published.ptr, xrt_str_from_cstr("child"), published_child);
    CHECK(xrt_execution_arena_live_objects(arena) == 3,
          "published test graph contains root, key, and child");
    published = xrt_value_set_storage(published, XR_OBJ_STORAGE_SHARED);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "shared publication atomically detaches the complete owned graph");
    xrt_execution_arena_restore(previous);
    xrt_execution_arena_destroy(arena);
    CHECK(((XrObjHeader *) published.ptr)->extra & XR_OBJ_AOT_ALLOCATION,
          "published object remains a valid prefixed allocation after arena teardown");
    xrt_release(published);

    arena = xrt_execution_arena_new();
    CHECK(arena != NULL, "enum publication arena is created");
    previous = xrt_execution_arena_enter(arena);
    enum_child = xrt_str_from_cstr("published-enum");
    enum_value = xrt_enum_aggregate_make(73, 1, 1, "Result", "Value", &enum_child);
    enum_box = xrt_enum_aggregate_box(enum_value);
    enum_box = xrt_value_set_storage(enum_box, XR_OBJ_STORAGE_SHARED);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "enum publication detaches both wrapper and payload graph");
    xrt_execution_arena_restore(previous);
    xrt_execution_arena_destroy(arena);
    CHECK(strcmp(xr_str_data(xrt_enum_field_get(enum_box, 1)), "published-enum") == 0,
          "published enum payload survives producer execution teardown");
    xrt_release(enum_box);

    arena = xrt_execution_arena_new();
    CHECK(arena != NULL, "native-class publication arena is created");
    previous = xrt_execution_arena_enter(arena);
    uint16_t box_type = xrt_type_register_hot(0, NULL, 0, arena_test_box_destroy,
                                              arena_test_box_promote, sizeof(ArenaTestBox));
    ArenaTestBox *box = (ArenaTestBox *) xrt_obj_alloc(box_type, sizeof(*box));
    box->child = xrt_str_from_cstr("native-field");
    XrValue published_box = xrt_box_obj(box);
    CHECK(published_box.ptr == (void *) &box->hdr,
          "native class uses the canonical header-at-value-pointer ABI");
    CHECK(box->hdr.type == XR_TINSTANCE,
          "native class header stores the canonical instance object kind");
    CHECK(xrt_aot_class_type_id(&box->hdr) == box_type,
          "native class id uses the disjoint AOT-local auxiliary domain");
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "native class and reference field start in the producer execution");
    published_box = xrt_value_set_storage(published_box, XR_OBJ_STORAGE_SHARED);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "generated native-class promoter detaches every reference field");
    xrt_execution_arena_restore(previous);
    xrt_execution_arena_destroy(arena);
    CHECK(((ArenaTestBox *) published_box.ptr)->child.ptr != NULL,
          "published native-class field survives producer teardown");
    xrt_release(published_box);

    arena = xrt_execution_arena_new();
    CHECK(arena != NULL, "transfer-tuple publication arena is created");
    previous = xrt_execution_arena_enter(arena);
    XrValue buffer = xrt_buffer_new(64, 1, 0);
    xrt_retain(buffer);
    XrValue transfer_tuple = xrt_tuple_make_storage(1, &buffer, XR_OBJ_STORAGE_TRANSFER);
    CHECK(xrt_arc_value_header(transfer_tuple) == (XrObjHeader *) transfer_tuple.ptr,
          "transfer tuple exposes the canonical header at its value pointer");
    CHECK((((XrObjHeader *) transfer_tuple.ptr)->extra & XR_OBJ_AOT_NATIVE) != 0,
          "transfer tuple advertises the backend-neutral native object ABI");
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "transfer tuple publication detaches its nested Buffer");
    CHECK(atomic_load_explicit(&xrt_arc_value_header(buffer)->refcount, memory_order_relaxed) == 1,
          "storage publication preserves both live Buffer owners");
    xrt_release(buffer);
    CHECK(atomic_load_explicit(&xrt_arc_value_header(buffer)->refcount, memory_order_relaxed) == 0,
          "releasing the source owner leaves the tuple owner live");
    xrt_execution_arena_restore(previous);
    xrt_execution_arena_destroy(arena);
    CHECK(xrt_buffer_length(((xrt_tuple_t *) transfer_tuple.ptr)->items[0]) == 64,
          "nested Buffer survives producer execution teardown");
    xrt_release(transfer_tuple);

    xrt_arc_shutdown();
    printf("execution arena tests passed (%d allocations, %d frees)\n", g_allocations, g_frees);
    return 0;
}
