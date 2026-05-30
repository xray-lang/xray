#include "../../../src/ir/xi_opt_comptime.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static int passed, failed;

static void test_null(void) {
    XiPassChange c = xi_opt_comptime_eval(NULL);
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
