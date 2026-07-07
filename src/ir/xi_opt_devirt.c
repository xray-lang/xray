#include "xi_opt_devirt.h"
#include "../base/xchecks.h"

/*
 * Devirtualization is now owned by global evidence -> XaotMethodDispatchPlan
 * -> verifier -> backend.  This legacy Xi pass intentionally performs no
 * local class/method inference so the optimizer cannot reintroduce a second
 * dispatch truth source.
 */
XR_FUNC XiPassChange xi_opt_devirt(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_devirt: NULL func");
    (void) f;
    return xi_pass_no_change();
}
