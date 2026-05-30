#include "../../../src/jit/xm_pic_emit.h"
#include "../../../src/jit/xm_pic.h"
#include <stdio.h>
#include <string.h>

static int passed, failed;

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d\n", #c, __LINE__);                                                  \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_megamorphic(void) {
    XmPic pic;
    xm_pic_init(&pic);
    pic.state = XM_PIC_MEGAMORPHIC;
    uint8_t buf[64];
    XmPicEmitResult res;
    uint32_t n = xm_pic_emit_x64(buf, sizeof(buf), &pic, 0, &res);
    ASSERT(n == 0);
    ASSERT(res.megamorphic_fallback);
    passed++;
}

static void test_mono_emit(void) {
    XmPic pic;
    xm_pic_init(&pic);
    xm_pic_record(&pic, 42, 0, (void *) 0x1000);
    uint8_t buf[128];
    XmPicEmitResult res;
    uint32_t n = xm_pic_emit_x64(buf, sizeof(buf), &pic, 16, &res);
#if defined(__x86_64__) || defined(_M_X64)
    ASSERT(n > 0);
    ASSERT(res.used_pic);
#else
    ASSERT(n == 0);
#endif
    passed++;
}

int main(void) {
    test_megamorphic();
    test_mono_emit();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
