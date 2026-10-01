/* An independently compiled obsolete provider must never be invoked. */
#include "xir_legacy_call_provider.h"
#include <stddef.h>
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 15 && XR_XIR_CALL_ABI_VERSION == 19, "old provider epochs");
static uint32_t entered, released;
static XrXirAction old_resume(XrXirCallView *view) {
    (void)view; ++entered;
    return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, {0}, {0}, 0};
}
static void old_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)view; (void)reason; ++released;
}
const XrXirCallEntry old_provider_entry = {19, NULL, 0, XR_XIR_UNIT, 0, old_resume, old_release, NULL, 0, 0};
uint32_t old_provider_callbacks(void) { return entered + released; }
size_t old_provider_entry_bytes(void) { return sizeof(XrXirCallEntry); }
