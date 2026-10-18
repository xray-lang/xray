/* Shared hosted value and entry ABI checks, independent of stdlib loading. */

#include "xray_hosted_fragment_abi.h"

_Static_assert(XR_HOSTED_OBJECT_ABI_VERSION == 1, "hosted object ABI changed");
_Static_assert(XR_HOSTED_FRAGMENT_ABI_VERSION == 7, "hosted fragment ABI changed");
_Static_assert(sizeof(XrValue) == 16, "hosted value layout changed");

int main(void) {
    return 0;
}
