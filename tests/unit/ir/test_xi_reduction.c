#include "../../../src/ir/xi_opt_reduction.h"
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
    XiPassChange c = xi_opt_reduction(NULL);
    ASSERT(!c.values_changed);
    passed++;
}

int main(void) {
    test_null();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
