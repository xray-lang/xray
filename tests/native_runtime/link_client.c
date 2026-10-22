/* The actual root explicitly consumes the strong image descriptor. */
#include "xray_native_runtime.h"
static void root(void *context) { (void)context; xrt_print_i64(42, 0); }
int main(void) {
    xrt_run_standalone(&xrt_standalone_source_map, root, NULL);
    return 0;
}
