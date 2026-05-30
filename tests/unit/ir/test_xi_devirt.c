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

static XiClassData *make_class_data_named(XiFunc *f, const char *class_name,
                                          const char *super_name) {
    XiClassData *data = (XiClassData *) xi_func_arena_alloc(f, sizeof(XiClassData));
    XiClassMethod *methods = (XiClassMethod *) xi_func_arena_alloc(f, 2 * sizeof(XiClassMethod));
    uint16_t *child_idx = (uint16_t *) xi_func_arena_alloc(f, 2 * sizeof(uint16_t));
    assert(data != NULL);
    assert(methods != NULL);
    assert(child_idx != NULL);
    memset(data, 0, sizeof(*data));
    memset(methods, 0, 2 * sizeof(*methods));

    data->class_name = class_name;
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

static XiClassData *make_class_data(XiFunc *f, const char *super_name) {
    return make_class_data_named(f, "Greeter", super_name);
}

static XiValue *make_method_call_on_class(XiFunc *f, XrType *ret_type, XrType *recv_type,
                                          int64_t aux_int, const char *class_name,
                                          const char *super_name) {
    XiBlock *entry = f->entry;
    XiValue *cls = xi_value_new(f, entry, XI_CLASS_CREATE, ret_type, 0);
    XiValue *recv = xi_param(f, entry, 0, recv_type);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, ret_type, 1);
    assert(cls != NULL);
    assert(recv != NULL);
    assert(call != NULL);

    cls->aux = make_class_data_named(f, class_name, super_name);
    call->args[0] = recv;
    call->aux = (void *) "run";
    call->aux_int = aux_int;
    return call;
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
    /* "Greeter" extends "Base" where Base.has_subclass=true.
     * BUT Greeter itself has no subclasses, so devirt on Greeter receivers
     * is safe — no subclass of Greeter can override "run". */
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
    assert(chg.values_changed);
    assert(call->op == XI_CALL_METHOD_DIRECT);
    xi_func_free(f);
}

/* Test: class with base AND has own subclasses — must reject. */
static void test_base_with_subclass_rejected(void) {
    XrClassInfo base_info = {.name = "Base", .has_subclass = true, .struct_layout = NULL};
    XrClassInfo info = {.name = "Greeter",
                        .base_name = "Base",
                        .base = &base_info,
                        .has_subclass = true,
                        .struct_layout = NULL};
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

static void test_cha_final_method_devirt(void) {
    XaMethodSlot vtable[2];
    memset(vtable, 0, sizeof(vtable));
    vtable[0].name = "skip";
    vtable[0].is_final = false;
    vtable[1].name = "run";
    vtable[1].is_final = true;

    XrClassInfo info = {
        .name = "Greeter",
        .has_subclass = true,
        .vtable = vtable,
        .vtable_size = 2,
        .struct_layout = NULL,
    };
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
    xi_func_free(f);
}

static void test_cha_nonfinal_method_rejected(void) {
    XaMethodSlot vtable[2];
    memset(vtable, 0, sizeof(vtable));
    vtable[0].name = "skip";
    vtable[0].is_final = true;
    vtable[1].name = "run";
    vtable[1].is_final = false;

    XrClassInfo info = {
        .name = "Greeter",
        .has_subclass = true,
        .vtable = vtable,
        .vtable_size = 2,
        .struct_layout = NULL,
    };
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
    xi_func_free(f);
}

static void test_cha_single_implementor_devirt(void) {
    XrClassInfo base_info = {
        .name = "Base",
        .has_subclass = true,
        .struct_layout = NULL,
    };
    XaMethodSlot child_vtable[1];
    memset(child_vtable, 0, sizeof(child_vtable));
    child_vtable[0].name = "run";
    child_vtable[0].is_final = false;

    XrClassInfo child_info = {
        .name = "Worker",
        .base_name = "Base",
        .base = &base_info,
        .vtable = child_vtable,
        .vtable_size = 1,
        .struct_layout = NULL,
    };

    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Base", .class_ref = &base_info}};
    XrType worker_type = {.kind = XR_KIND_INSTANCE,
                          .id = 3,
                          .frozen = true,
                          .instance = {.class_name = "Worker", .class_ref = &child_info}};
    XiFunc *f = make_func(&ret_type);
    (void) xi_param(f, f->entry, 1, &worker_type);
    XiValue *call = make_method_call_on_class(f, &ret_type, &recv_type, 0, "Worker", NULL);

    XiPassChange chg = xi_opt_devirt(f);
    assert(chg.values_changed);
    assert(call->op == XI_CALL_METHOD_DIRECT);
    assert(call->aux_int == 1);
    xi_func_free(f);
}

static void test_cha_no_vtable_rejected(void) {
    XrClassInfo info = {
        .name = "Greeter",
        .has_subclass = true,
        .vtable = NULL,
        .vtable_size = 0,
        .struct_layout = NULL,
    };
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
    xi_func_free(f);
}

/* Test: inherited method devirt — method defined in parent class,
 * invoked on child class without override. */
static void test_inherited_method_devirt(void) {
    XrClassInfo base_info = {.name = "Animal", .has_subclass = false, .struct_layout = NULL};
    XrClassInfo info = {.name = "Dog",
                        .base_name = "Animal",
                        .base = &base_info,
                        .has_subclass = false,
                        .struct_layout = NULL};
    XrType ret_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
    XrType recv_type = {.kind = XR_KIND_INSTANCE,
                        .id = 2,
                        .frozen = true,
                        .instance = {.class_name = "Dog", .class_ref = &info}};
    XiFunc *f = make_func(&ret_type);

    /* Dog class data: no "run" method — only parent Animal has it. */
    XiBlock *entry = f->entry;
    XiValue *cls_dog = xi_value_new(f, entry, XI_CLASS_CREATE, &ret_type, 0);
    XiClassData *dog_data = make_class_data_named(f, "Dog", "Animal");
    dog_data->methods[1].name = "bark"; /* Dog defines "bark", not "run" */
    cls_dog->aux = dog_data;

    /* Animal class data: defines "run" */
    XiValue *cls_animal = xi_value_new(f, entry, XI_CLASS_CREATE, &ret_type, 0);
    cls_animal->aux = make_class_data_named(f, "Animal", NULL);

    XiValue *recv = xi_param(f, entry, 0, &recv_type);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &ret_type, 1);
    call->args[0] = recv;
    call->aux = (void *) "run";
    call->aux_int = 0;

    XiPassChange chg = xi_opt_devirt(f);
    /* Should devirt: Dog inherits "run" from Animal (no subclass, no override). */
    assert(chg.values_changed);
    assert(call->op == XI_CALL_METHOD_DIRECT);
    xi_func_free(f);
}

int main(void) {
    test_direct_call();
    test_subclass_rejected();
    test_super_call_rejected();
    test_base_class_rejected();
    test_base_with_subclass_rejected();
    test_cha_final_method_devirt();
    test_cha_nonfinal_method_rejected();
    test_cha_single_implementor_devirt();
    test_cha_no_vtable_rejected();
    test_inherited_method_devirt();
    fprintf(stderr, "xi devirt tests passed\n");
    return 0;
}
