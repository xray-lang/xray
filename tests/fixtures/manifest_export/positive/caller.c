#include <stdint.h>
#include "exports.h"

int main(void) {
    return xr_add_i32(INT32_C(19), INT32_C(23)) == INT32_C(42) ? 0 : 1;
}
