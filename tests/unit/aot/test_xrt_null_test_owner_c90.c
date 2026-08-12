/* Restricted C90 null-test mechanical adapter KAT. */

#include "aot/xrt_c90.h"

int main(void) {
    XrValue tagged = xrt_c90_null_value();
    int marker = 1;
    if (!xrt_null_test_tagged(tagged.tag))
        return 1;
    tagged.tag = 1;
    if (xrt_null_test_tagged(tagged.tag))
        return 2;
    if (!xrt_null_test_pointer(NULL))
        return 3;
    if (xrt_null_test_pointer(&marker))
        return 4;
    return 0;
}
