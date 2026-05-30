#include "../../../src/ir/xi_opt_loop_vec.h"
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
    XiPassChange c = xi_opt_loop_vec(NULL);
    ASSERT(!c.values_changed);
    passed++;
}

static void test_empty(void) {
    XiFunc *f = xi_func_new("t", &stub_int);
    XiBlock *e = xi_block_new(f);
    XiValue *c = xi_const_int(f, e, 0, &stub_int);
    xi_block_set_return(e, c);
    XiPassChange chg = xi_opt_loop_vec(f);
    ASSERT(!chg.values_changed);
    xi_func_free(f);
    passed++;
}

int main(void) {
    test_null();
    test_empty();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
