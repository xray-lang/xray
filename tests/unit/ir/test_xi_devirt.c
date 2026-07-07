#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt_devirt.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void test_legacy_devirt_pass_does_not_rewrite_method_calls(void) {
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE, .id = 2, .frozen = true};
    XiFunc *f = xi_func_new("legacy_devirt_noop", &ret_type);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);
    XiValue *recv = xi_param(f, entry, 0, &recv_type);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &ret_type, 1);
    assert(recv != NULL);
    assert(call != NULL);

    call->args[0] = recv;
    call->aux = (void *) "run";
    call->aux_int = 24;

    XiPassChange chg = xi_opt_devirt(f);
    if (chg.values_changed || chg.cfg_changed) {
        fprintf(stderr, "legacy devirt pass unexpectedly changed IR\n");
        abort();
    }
    assert(call->op == XI_CALL_METHOD);
    assert(call->aux_int == 24);

    xi_func_free(f);
}

int main(void) {
    test_legacy_devirt_pass_does_not_rewrite_method_calls();
    fprintf(stderr, "xi legacy devirt noop tests passed\n");
    return 0;
}
