/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_call_cases.h - A preexisting callback cannot hide a loan's mutable root
 */
#include "lir/xlir_internal.h"
#include "lir/xlir_callable.h"
#include "lir/xlir_view.h"
static XrLirFunction slice_state_callback(CArrayFixture *c, XrLirType callable) {
    XrLirType parameter = XR_LIR_STATE; XrLirFunction target = {0};
    XrLirFunctionSpec signature = {.name="mutate",.name_length=6,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c->module, &signature, &target));
    OK(xr_lir_set_closure_target(c->module, target, (XrLirClosureTarget){.callable=callable,.state_capture=true}));
    OK(xr_lir_define_function(c->module, target)); XrLirValue state = {0};
    OK(xr_lir_function_parameter(c->module, target, 0, &state)); XrLirBlock block = {0};
    OK(xr_lir_add_block(c->module, target, NULL, 0, &block)); OK(xr_lir_set_entry(c->module, target, block));
    XrLirValue reference = {0}; XrLirInstruction ref = {.op=XR_LIR_STATE_REFERENCE,.type=XR_LIR_STATE_REF,.operands=&state,.operand_count=1};
    OK(xr_lir_emit_value(c->module, block, &ref, &reference));
    XrLirValue binding = ca_value(c, block, XR_LIR_ARRAY_BORROW, XR_LIR_ARRAY_REF, &reference, 1);
    slice_write(c, block, binding, false); ca_return(c, block, ca_constant(c, block, 99));
    return target;
}
static XrLirFunction slice_callback_wrapper(CArrayFixture *c, XrLirType callable) {
    XrLirFunction wrapper = {0};
    XrLirFunctionSpec signature = {.name="use_callback",.name_length=12,.parameters=&callable,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c->module, &signature, &wrapper)); OK(xr_lir_define_function(c->module, wrapper));
    XrLirValue callback = {0}; OK(xr_lir_function_parameter(c->module, wrapper, 0, &callback));
    XrLirBlock block = {0}; OK(xr_lir_add_block(c->module, wrapper, NULL, 0, &block));
    OK(xr_lir_set_entry(c->module, wrapper, block));
    ca_return(c, block, ca_value(c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, &callback, 1));
    return wrapper;
}
static XrLirModule *slice_callback_fixture(XrLirType element, bool ended, XrLirFunction *root) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL, &c.module)); OK(xr_lir_slice_catalog_install(c.module));
    XrLirType callable = 0, slice = 0; XrLirCallableType callback_type = {.result=XR_LIR_I64};
    OK(xr_lir_callable_type(c.module, &callback_type, &callable)); OK(xr_lir_slice_type(c.module, element, &slice));
    XrLirStateModule module = {0}; XrLirStateSlot slot = {.payload=XR_LIR_ARRAY,.mutable=true,.size=8,.alignment=8,.array_element=element};
    XrLirStateLayout layout = {.modules=&module,.module_count=1,.slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8};
    OK(xr_lir_set_state_layout(c.module, &layout));
    XrLirFunction target = slice_state_callback(&c, callable), wrapper = slice_callback_wrapper(&c, callable);
    XrLirType parameter = XR_LIR_STATE;
    XrLirFunctionSpec signature = {.name="root",.name_length=4,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &signature, root)); OK(xr_lir_set_state_entry(c.module, *root));
    OK(xr_lir_define_function(c.module, *root)); XrLirValue state = {0};
    OK(xr_lir_function_parameter(c.module, *root, 0, &state)); XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &block)); OK(xr_lir_set_entry(c.module, *root, block));
    XrLirInstruction boundary = {.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1};
    OK(xr_lir_emit_effect(c.module, block, &boundary));
    XrLirValue initial = number(&c, block, element, 42);
    XrLirValue array = role(&c, block, XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY, &initial, 1), args[] = {state,array}, reference = {0};
    XrLirInstruction construct = {.op=XR_LIR_STATE_CONSTRUCT,.type=XR_LIR_STATE_REF,.operands=args,.operand_count=2};
    OK(xr_lir_emit_value(c.module, block, &construct, &reference)); ca_effect(&c, block, XR_LIR_ARRAY_DROP, array);
    ca_effect(&c, block, XR_LIR_STATE_PUBLISH, reference); boundary.op = XR_LIR_MODULE_READY;
    OK(xr_lir_emit_effect(c.module, block, &boundary));
    XrLirValue callback = {0}; XrLirInstruction ref = {.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,.operands=&state,.operand_count=1};
    OK(xr_lir_emit_value(c.module, block, &ref, &callback));
    XrLirValue binding = ca_value(&c, block, XR_LIR_ARRAY_BORROW, XR_LIR_ARRAY_REF, &reference, 1);
    XrLirValue range[] = {binding,ca_constant(&c,block,0),ca_constant(&c,block,1)};
    XrLirValue view = slice_role(&c, block, XR_LIR_SLICE_OP_NEW, slice, range, 3);
    XrLirValue length = slice_role(&c, block, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, &view, 1);
    if (ended) (void)slice_role(&c, block, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &view, 1);
    XrLirInstruction call = {.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=wrapper,.operands=&callback,.operand_count=1};
    XrLirValue result = {0}; OK(xr_lir_emit_value(c.module, block, &call, &result));
    if (!ended) (void)slice_role(&c, block, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &view, 1);
    ca_return(&c, block, length); return c.module;
}
static void slice_callback_frontier(void) {
    const XrLirType elements[] = {XR_LIR_I64,XR_LIR_U8};
    for (size_t family = 0; family < 2; ++family) {
        XrLirFunction root = {0}; XrLirModule *module = slice_callback_fixture(elements[family], false, &root);
        XrLirDiagnostic diagnostic = {0};
        CHECK(xr_lir_module_finish(module, &diagnostic) == XR_LIR_UNSUPPORTED && diagnostic.function == root.id &&
            strcmp(diagnostic.message, "callable arguments with a live Slice require captured binding effect facts") == 0);
        CHECK(diagnostic.block && diagnostic.block <= module->functions[root.id - 1].block_count);
        const XrLirBlockData *block = &module->functions[root.id - 1].blocks[diagnostic.block - 1];
        CHECK(diagnostic.instruction && diagnostic.instruction <= block->instruction_count);
        XrLirInstructionView rejected = block->instructions[diagnostic.instruction - 1].view;
        CHECK(rejected.op == XR_LIR_CALL && rejected.operand_count == 1 && rejected.callee == 2);
        xr_lir_module_destroy(module); root = (XrLirFunction){0};
        module = slice_callback_fixture(elements[family], true, &root); OK(xr_lir_module_finish(module, NULL));
        xr_lir_module_destroy(module);
    }
}
