#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/ir/xi_opt_devirt.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static XiClassData *make_class_data(XiFunc *f, const char *super_name) {
    XiClassData *data = (XiClassData *) xi_func_arena_alloc(f, sizeof(XiClassData));
    XiClassMethod *methods = (XiClassMethod *) xi_func_arena_alloc(f, 2 * sizeof(XiClassMethod));
    uint16_t *child_idx = (uint16_t *) xi_func_arena_alloc(f, 2 * sizeof(uint16_t));
    assert(data != NULL);
    assert(methods != NULL);
    assert(child_idx != NULL);
    memset(data, 0, sizeof(*data));
    memset(methods, 0, 2 * sizeof(*methods));

    data->class_name = "Greeter";
    data->super_name = super_name;
    data->methods = methods;
    data->child_idx = child_idx;
    data->nmethod = 2;
    data->ninst = 2;
    methods[0].name = "skip";
    methods[1].name = "run";
    child_idx[0] = 0;
    child_idx[1] = 1;
    return data;
}

static XiValue *make_method_call_with_super(XiFunc *f, XrType *ret_type, XrType *recv_type,
                                            int64_t aux_int, const char *super_name) {
    XiBlock *entry = f->entry;
    XiValue *cls = xi_value_new(f, entry, XI_CLASS_CREATE, ret_type, 0);
    XiValue *recv = xi_param(f, entry, 0, recv_type);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, ret_type, 1);
    assert(cls != NULL);
    assert(recv != NULL);
    assert(call != NULL);

    cls->aux = make_class_data(f, super_name);
    call->args[0] = recv;
    call->aux = (void *) "run";
    call->aux_int = aux_int;
    return call;
}

static XiValue *make_method_call(XiFunc *f, XrType *ret_type, XrType *recv_type, int64_t aux_int) {
    return make_method_call_with_super(f, ret_type, recv_type, aux_int, NULL);
}

static XiFunc *make_func(XrType *ret_type) {
    XiFunc *f = xi_func_new("devirt", ret_type);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);
    entry->sealed = true;

    f->children = (XiFunc **) xr_calloc(2, sizeof(XiFunc *));
    assert(f->children != NULL);
    f->children[0] = xi_func_new("skip", ret_type);
    f->children[1] = xi_func_new("run", ret_type);
    assert(f->children[0] != NULL);
    assert(f->children[1] != NULL);
    f->nchildren = 2;
    f->children_cap = 2;
    return f;
}

static void test_direct_call(void) {
    XrClassInfo info = {.name = "Greeter", .has_subclass = false, .struct_layout = NULL};
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Greeter", .class_ref = &info}};
    XiFunc *f = make_func(&ret_type);
    XiValue *call = make_method_call(f, &ret_type, &recv_type, 0);

    XiPassChange chg = xi_opt_devirt(f);
    assert(chg.values_changed);
    assert(call->op == XI_CALL_METHOD_DIRECT);
    assert(call->aux_int == 1);
    assert((call->flags & xi_op_default_effects(XI_CALL_METHOD_DIRECT)) ==
           xi_op_default_effects(XI_CALL_METHOD_DIRECT));
    assert(call->mem_group == XI_MEM_TOP);
    xi_func_free(f);
}

static void test_subclass_rejected(void) {
    XrClassInfo info = {.name = "Greeter", .has_subclass = true, .struct_layout = NULL};
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Greeter", .class_ref = &info}};
    XiFunc *f = make_func(&ret_type);
    XiValue *call = make_method_call(f, &ret_type, &recv_type, 0);

    XiPassChange chg = xi_opt_devirt(f);
    assert(!chg.values_changed);
    assert(call->op == XI_CALL_METHOD);
    assert(call->aux_int == 0);
    xi_func_free(f);
}

static void test_super_call_rejected(void) {
    XrClassInfo info = {.name = "Greeter", .has_subclass = false, .struct_layout = NULL};
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Greeter", .class_ref = &info}};
    XiFunc *f = make_func(&ret_type);
    XiValue *call = make_method_call(f, &ret_type, &recv_type, 1);

    XiPassChange chg = xi_opt_devirt(f);
    assert(!chg.values_changed);
    assert(call->op == XI_CALL_METHOD);
    assert(call->aux_int == 1);
    xi_func_free(f);
}

static void test_base_class_rejected(void) {
    XrClassInfo base_info = {.name = "Base", .has_subclass = true, .struct_layout = NULL};
    XrClassInfo info = {
        .name = "Greeter", .base_name = "Base", .base = &base_info, .struct_layout = NULL};
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Greeter", .class_ref = &info}};
    XiFunc *f = make_func(&ret_type);
    XiValue *call = make_method_call_with_super(f, &ret_type, &recv_type, 0, "Base");

    XiPassChange chg = xi_opt_devirt(f);
    assert(!chg.values_changed);
    assert(call->op == XI_CALL_METHOD);
    xi_func_free(f);
}

int main(void) {
    test_direct_call();
    test_subclass_rejected();
    test_super_call_rejected();
    test_base_class_rejected();
    fprintf(stderr, "xi devirt tests passed\n");
    return 0;
}
