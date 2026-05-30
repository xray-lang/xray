#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_backend_lower.h"
#include "../../../src/ir/xi_vec_scalar_lower.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static void test_vec_store_scalarized(void) {
    XiFunc *f = xi_func_new("vec_scalar", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    f->stage = XI_STAGE_REPPED;

    XiValue *arr_a = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *arr_b = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *arr_c = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *start = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    start->aux_int = 2;

    XiValue *vec_b = xi_value_new(f, entry, XI_VEC_LOAD, &stub_int, 2);
    vec_b->args[0] = arr_b;
    vec_b->args[1] = start;
    vec_b->aux_int = 2;

    XiValue *vec_c = xi_value_new(f, entry, XI_VEC_LOAD, &stub_int, 2);
    vec_c->args[0] = arr_c;
    vec_c->args[1] = start;
    vec_c->aux_int = 2;

    XiValue *vec_add = xi_value_new(f, entry, XI_VEC_ADD, &stub_int, 2);
    vec_add->args[0] = vec_b;
    vec_add->args[1] = vec_c;
    vec_add->aux_int = 2;

    XiValue *vec_store = xi_value_new(f, entry, XI_VEC_STORE, &stub_int, 3);
    vec_store->args[0] = arr_a;
    vec_store->args[1] = vec_add;
    vec_store->args[2] = start;
    vec_store->aux_int = 2;

    xi_block_set_return(entry, vec_store);

    assert(xi_vec_scalar_lower(f));
    assert(vec_store->op == XI_COPY);

    uint32_t index_sets = 0;
    for (uint32_t vi = 0; vi < entry->nvalues; vi++) {
        XiValue *v = entry->values[vi];
        if (v && v->op == XI_INDEX_SET)
            index_sets++;
    }
    assert(index_sets == 2);

    xi_func_free(f);
}

int main(void) {
    test_vec_store_scalarized();
    fprintf(stderr, "xi vec scalar tests passed\n");
    return 0;
}
