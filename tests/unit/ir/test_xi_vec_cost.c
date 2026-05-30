#include "../../../src/ir/xi_vec_cost.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static int passed, failed;

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d\n", #c, __LINE__);                                                  \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_null(void) {
    ASSERT(!xi_vec_loop_profitable(NULL, 4));
    passed++;
}

static void test_short_trip(void) {
    XiLoop loop = {0};
    loop.has_trip_count = true;
    loop.trip_count = 4;
    ASSERT(!xi_vec_loop_profitable(&loop, 4));
    passed++;
}

static void test_long_trip(void) {
    XiFunc *f = xi_func_new("loop_cost", &stub_int);
    XiBlock *body = xi_block_new(f);
    (void) xi_const_int(f, body, 1, &stub_int);

    XiLoop loop = {0};
    loop.has_trip_count = true;
    loop.trip_count = 32;
    XiBlock *body_ptr = body;
    loop.body = &body_ptr;
    loop.nbody = 1;
    loop.nbasic_ivs = 1;

    ASSERT(xi_vec_loop_profitable(&loop, 4));
    xi_func_free(f);
    passed++;
}

int main(void) {
    test_null();
    test_short_trip();
    test_long_trip();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
