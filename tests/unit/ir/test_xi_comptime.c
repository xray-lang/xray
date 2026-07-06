#include "../../../src/ir/xi_opt_comptime.h"
#include "../../../src/ir/xi.h"
#include <stdio.h>

static int passed, failed;

static void test_null(void) {
    XiPassChange c = xi_opt_const_fixpoint(NULL);
    if (c.values_changed)
        failed++;
    else
        passed++;
}

int main(void) {
    test_null();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
