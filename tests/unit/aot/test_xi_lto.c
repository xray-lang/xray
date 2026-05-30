#include "../../../src/aot/xi_lto.h"
#include <stdio.h>

static int passed, failed;

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d\n", #c, __LINE__);                                                  \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_init_fail(void) {
    XiLtoContext ctx;
    ASSERT(!xi_lto_context_init(&ctx, NULL, 0));
    passed++;
}

int main(void) {
    test_init_fail();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
