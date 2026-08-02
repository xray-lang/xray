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

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    (void) exc;
    abort();
}

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", message);                                               \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct ArenaTestBox {
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
    xrt_release(scalar);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "normal RC release unlinks and reclaims an acyclic object");

    XrValue tuple_child = xrt_str_from_cstr("tuple-owned");
    XrValue tuple = xrt_tuple_make(1, &tuple_child);
    xrt_release(tuple_child);
    CHECK(xrt_execution_arena_live_objects(arena) == 2,
          "tuple takes an owning reference to each reference-valued lane");
    xrt_release(tuple);
    CHECK(xrt_execution_arena_live_objects(arena) == 0,
          "tuple destruction releases its owned lanes and allocation");

    XrValue left = xrt_cell_new(XR_NULL_VAL);
    XrValue right = xrt_cell_new(XR_NULL_VAL);
    xrt_retain(right);
    xrt_cell_set(left, right);
    xrt_retain(left);
    xrt_cell_set(right, left);
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
    CHECK(arena != NULL, "native-class publication arena is created");
    previous = xrt_execution_arena_enter(arena);
    uint16_t box_type = xrt_type_register_hot(0, NULL, 0, arena_test_box_destroy,
                                               arena_test_box_promote, sizeof(ArenaTestBox));
    ArenaTestBox *box = (ArenaTestBox *) xrt_obj_alloc(box_type, sizeof(*box));
    box->child = xrt_str_from_cstr("native-field");
    XrValue published_box = xrt_box_obj(box);
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

    xrt_arc_shutdown();
    printf("execution arena tests passed (%d allocations, %d frees)\n", g_allocations, g_frees);
    return 0;
}
