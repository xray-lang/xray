#include "../../../src/ir/xi_opt_call_specialize.h"
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

static void test_const_args(void) {
    XiFunc *f = xi_func_new("t", &stub_int);
    XiBlock *e = xi_block_new(f);
    XiValue *callee = xi_const_int(f, e, 0, &stub_int);
    XiValue *arg = xi_const_int(f, e, 42, &stub_int);
    XiValue *call = xi_value_new(f, e, XI_CALL, &stub_int, 2);
    call->args[0] = callee;
    call->args[1] = arg;
    xi_block_set_return(e, call);
    (void) xi_opt_call_specialize(f);
    ASSERT((call->flags & (1u << 6)) != 0);
    xi_func_free(f);
    passed++;
}

int main(void) {
    test_const_args();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
