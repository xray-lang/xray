/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "vm/register/xrbc_internal.h"
static void callable_scope_verification(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_factory_fixture_mode(&module, &root, false, true);
    XrLirModule *rebuilt = NULL, *inlined = NULL, *folded = NULL;
    OK(xr_lir_rebuild(module, &rebuilt));
    OK(xr_lir_inline(rebuilt, &inlined));
    OK(xr_lir_sccp(inlined, &folded));
    xr_lir_module_destroy(rebuilt);
    xr_lir_module_destroy(inlined);
    OK(xr_lir_verify(folded, NULL));
    xr_lir_module_destroy(folded);
    callable_rbc_unavailable(module, root); callable_c_supported(module,root,true);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(module, &copy));
        XrLirFunctionData *function = &copy->functions[mutation == 5 ? 1 : root.id - 1];
        XrLirBlockData *block = &function->blocks[function->entry - 1];
        XrLirInstructionView *first_scope = NULL, *temporary = NULL, *bind = NULL, *call = NULL, *invoke = NULL, *reference = NULL;
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            XrLirInstructionView *v = &block->instructions[n].view;
            if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER) {
                if (!first_scope) first_scope = v;
                else if (!temporary) temporary = v;
            }
            if (v->op == XR_LIR_FUNCTION_BIND && !bind) bind = v;
            if (v->op == XR_LIR_CALL && !call) call = v;
            if (v->op == XR_LIR_CALL_INDIRECT && !invoke) invoke = v;
            if (v->op == XR_LIR_FUNCTION_REF) reference = v;
        }
        CHECK(first_scope && temporary);
        XrLirInstructionView saved_temporary = *temporary;
        if (mutation == 0) ((uint32_t *)temporary->operands)[0] = temporary->result;
        if (mutation == 1) {
            for (uint32_t n = 0; n < block->instruction_count; ++n) {
                XrLirInstructionView *v = &block->instructions[n].view;
                if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE && v->operands[0] == temporary->result) {
                    ((uint32_t *)v->operands)[0] = first_scope->result;
                    break;
                }
            }
        }
        if (mutation == 2) {
            temporary->op = XR_LIR_CONST;
            temporary->operands = NULL;
            temporary->operand_count = 0;
        }
        if (mutation == 3) {
            CHECK(call && invoke);
            ((uint32_t *)invoke->operands)[0] = call->result;
        }
        if (mutation == 4) {
            CHECK(bind && invoke);
            ((uint32_t *)invoke->operands)[0] = temporary->result;
        }
        if (mutation == 5) {
            CHECK(reference);
            ((uint32_t *)block->instructions[block->instruction_count - 1].view.operands)[0] = reference->result;
        }
        if (mutation == 6) {
            CHECK(bind);
            for (uint32_t n = 0; n < block->instruction_count; ++n) {
                XrLirInstructionView *v = &block->instructions[n].view;
                if (v->op == XR_LIR_FUNCTION_BIND && v != bind) v->bits = bind->bits;
            }
        }
        CHECK(xr_lir_verify(copy, NULL) == XR_LIR_VERIFY_ERROR);
        if (mutation == 2) *temporary = saved_temporary;
        xr_lir_module_destroy(copy);
    }
    xr_lir_module_destroy(module);
    puts("function scope capabilities survive transforms and reject seven stale or forged uses");
}
static void callable_owned_factory_cases(void) {
    callable_scope_verification();
    for (unsigned frame = 0; frame < 2; ++frame) {
        XrLirModule *module = NULL;
        XrLirFunction root;
        callable_factory_fixture(&module, &root, frame != 0);
        callable_rbc_unavailable(module, root);
        if (frame) callable_c_rejected(module,root,XR_LIR_C_UNSUPPORTED);
        else callable_c_supported(module,root,true);
        xr_lir_module_destroy(module);
    }
    XrLirModule *borrowed = NULL; XrLirFunction borrowed_root = {0};
    callable_c_borrowed_result_fixture(&borrowed,&borrowed_root);
    callable_c_supported(borrowed,borrowed_root,true); xr_lir_module_destroy(borrowed);
    puts("Owned factories have native lifetime transport; escaping Frame captures remain rejected");
}
