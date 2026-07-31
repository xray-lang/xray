#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int64_t xray_top_var_fixed_array_static(int64_t cpu, int64_t lane);

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

static int check_call(int call, int64_t cpu, int64_t lane, int64_t expected) {
    int64_t actual = xray_top_var_fixed_array_static(cpu, lane);
    if (actual == expected)
        return 0;
    fprintf(stderr,
            "freestanding static fixed-array mutation call %d returned %lld, expected %lld\n", call,
            (long long) actual, (long long) expected);
    return 1;
}

int main(void) {
    if (check_call(1, 0, 0, 67) != 0)
        return 1;
    if (check_call(2, 0, 0, 82) != 0)
        return 1;
    if (check_call(3, 1, 1, 78) != 0)
        return 1;
    if (check_call(4, 0, 0, 97) != 0)
        return 1;
    return 0;
}
