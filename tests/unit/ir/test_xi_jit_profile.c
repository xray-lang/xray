#include "../../../src/ir/xi_jit_profile.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt_spec_narrow.h"
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
    ASSERT(!xi_jit_apply_profile(NULL, NULL));
    passed++;
}

static void test_attach_empty_ic(void) {
    XiFunc *f = xi_func_new("t", &stub_int);
    XiBlock *e = xi_block_new(f);
    XiValue *c = xi_const_int(f, e, 0, &stub_int);
    xi_block_set_return(e, c);
    XiJitProfileInput p = {0};
    (void) xi_jit_apply_profile(f, &p);
    ASSERT(f->invariant_mask & XI_INV_IC_ATTACHED);
    xi_func_free(f);
    passed++;
}

int main(void) {
    test_null();
    test_attach_empty_ic();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
