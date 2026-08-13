/* Restricted C90 assertion-condition mechanical adapter KAT. */

#include "aot/xrt_c90.h"

int main(void) {
    if (xrt_assert_condition_failed(true, true))
        return 1;
    if (!xrt_assert_condition_failed(false, true))
        return 2;
    if (xrt_assert_condition_failed(false, false))
        return 3;
    if (!xrt_assert_condition_failed(true, false))
        return 4;
    return 0;
}
