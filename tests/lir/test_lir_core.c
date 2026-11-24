/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_core.c - Scalar SSA construction, ownership and rejection tests
 */
#include "lir_test_programs.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "base/xmalloc.h"
#include "lir_slice_callable_fixture.h"
#include "lir_slice_callback_loan_fixture.h"

static XrLirType try_error_type(XrLirModule *m,const char *name) {
    XrLirRecordIdentity identity={.module="fixture",.module_length=7,.name=name,.name_length=(uint32_t)strlen(name)};
    XrLirRecordField field={.name="code",.name_length=4,.type=XR_LIR_I64};
    XrLirSumVariant variants[]={{.name="Missing",.name_length=7},
        {.name="Bad",.name_length=3,.field_count=1}};
    XrLirType type=XR_LIR_TYPE_INVALID;
    OK(xr_lir_error_sum_type(m,&identity,&field,1,variants,2,&type));
    return type;
}

static void typed_error_cannot_be_erased(void) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirType error=try_error_type(m,"Error"),callable=0;
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_UNIT},&callable));
    XrLirFunction f={0};
    OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="leaf",.name_length=4,
        .result=XR_LIR_UNIT,.error=error},&f));
    OK(xr_lir_define_function(m,f));
    CHECK(xr_lir_set_closure_target(m,f,(XrLirClosureTarget){.callable=callable})==XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);

    m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    error=try_error_type(m,"Error");
    OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="leaf",.name_length=4,
        .result=XR_LIR_UNIT,.error=error},&f));
    XrLirFunction caller=test_function(m,"caller",NULL,0,XR_LIR_UNIT);
    XrLirBlock body=test_block(m,caller,NULL,0);
    OK(xr_lir_set_entry(m,caller,body));
    CHECK(xr_lir_emit_effect(m,body,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,
        .callee=f})==XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);

    m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirRecordArgument parameter={.type=XR_LIR_I64};
    XrLirRecordIdentity generic={.name="Generic",.name_length=7,.arguments=&parameter,.argument_count=1};
    XrLirSumVariant variant={.name="Bad",.name_length=3};
    error=0;
    CHECK(xr_lir_error_sum_type(m,&generic,NULL,0,&variant,1,&error)==XR_LIR_INVALID_ARGUMENT && !error);
    xr_lir_module_destroy(m);
}

static void invoke_rejects_forged_error(void) {
    for (unsigned mode=0;mode<3;++mode) {
        XrLirModule *m=NULL;
        OK(xr_lir_module_create(NULL,&m));
        XrLirType first=try_error_type(m,"First"),second=try_error_type(m,"Second");
        CHECK(first!=second && first==try_error_type(m,"First"));
        XrLirFunction f={0};
        XrLirBlock block={0};
        XrLirFunctionSpec spec={.name="probe",.name_length=5,.result=XR_LIR_UNIT,.error=first};
        OK(xr_lir_declare_function(m,&spec,&f));
        OK(xr_lir_define_function(m,f));
        OK(xr_lir_add_block(m,f,NULL,0,&block));
        OK(xr_lir_set_entry(m,f,block));
        XrLirValue value={0};
        XrLirInstruction create={.op=XR_LIR_SUM_NEW,.type=second,.bits=mode==2 ? 2u : 0u};
        if (mode==2) {
            CHECK(xr_lir_emit_value(m,block,&create,&value)==XR_LIR_VERIFY_ERROR && !value.id);
        } else {
            OK(xr_lir_emit_value(m,block,&create,&value));
            XrLirInstruction term={.op=mode==0 ? XR_LIR_RETURN_ERROR : XR_LIR_PANIC_ERROR,
                .type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
            if (mode==1) {
                XrLirLiteral literal={0};
                OK(xr_lir_error_literal(m,first,&literal));
                term.bits=literal.id;
                term.source=(XrLirSource){"test.xr",7,1,1,1,5};
            }
            CHECK(xr_lir_set_terminator(m,block,&term)==XR_LIR_VERIFY_ERROR);
        }
        xr_lir_module_destroy(m);
    }
}

static void invoke_must_forward_its_received_error(void) {
    enum { PROPAGATE, FORGED_PROPAGATION, CATCH, FORGED_CATCH, CASE_COUNT };
    for (unsigned mode=PROPAGATE;mode<CASE_COUNT;++mode) {
        XrLirModule *module=NULL;
        OK(xr_lir_module_create(NULL,&module));
        XrLirType error=try_error_type(module,"Failure");
        XrLirFunction leaf={0},caller={0};
        XrLirFunctionSpec spec={.name="leaf",.name_length=4,.result=XR_LIR_UNIT,.error=error};
        OK(xr_lir_declare_function(module,&spec,&leaf));
        OK(xr_lir_define_function(module,leaf));
        XrLirBlock body=test_block(module,leaf,NULL,0);
        OK(xr_lir_set_entry(module,leaf,body));
        XrLirValue payload=test_constant(module,body,XR_LIR_I64,23),value={0};
        XrLirInstruction create={.op=XR_LIR_SUM_NEW,.type=error,.bits=1,.operands=&payload,.operand_count=1};
        OK(xr_lir_emit_value(module,body,&create,&value));
        XrLirInstruction failed={.op=XR_LIR_RETURN_ERROR,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
        OK(xr_lir_set_terminator(module,body,&failed));

        spec.name="caller";
        spec.name_length=6;
        OK(xr_lir_declare_function(module,&spec,&caller));
        OK(xr_lir_define_function(module,caller));
        XrLirBlock call=test_block(module,caller,NULL,0);
        XrLirBlock success=test_block(module,caller,NULL,0);
        XrLirBlock error_edge=test_block(module,caller,&error,1);
        OK(xr_lir_set_entry(module,caller,call));
        XrLirValue forged={0};
        if (mode==FORGED_CATCH)
            OK(xr_lir_emit_value(module,call,&(XrLirInstruction){.op=XR_LIR_SUM_NEW,.type=error},&forged));
        XrLirInstruction invoke={.op=XR_LIR_INVOKE,.type=XR_LIR_UNIT,.callee=leaf,
            .edges={{.target=success},{.target=error_edge}},.edge_count=2};
        OK(xr_lir_set_terminator(module,call,&invoke));
        test_return(module,success,(XrLirValue){0});
        OK(xr_lir_block_parameter(module,error_edge,0,&value));
        if (mode==FORGED_PROPAGATION)
            OK(xr_lir_emit_value(module,error_edge,&(XrLirInstruction){.op=XR_LIR_SUM_NEW,.type=error},&value));
        if (mode==CATCH || mode==FORGED_CATCH) {
            XrLirBlock handler=test_block(module,caller,&error,1);
            XrLirValue argument=mode==FORGED_CATCH ? forged : value;
            test_branch(module,error_edge,handler,&argument,1);
            OK(xr_lir_block_parameter(module,handler,0,&value));
            OK(xr_lir_set_terminator(module,handler,&failed));
        } else {
            OK(xr_lir_set_terminator(module,error_edge,&failed));
        }
        XrLirDiagnostic diagnostic={0};
        XrLirStatus result=xr_lir_module_finish(module,&diagnostic);
        if (mode==FORGED_PROPAGATION || mode==FORGED_CATCH)
            CHECK(result==XR_LIR_VERIFY_ERROR && strstr(diagnostic.message,"dedicated normal and error successors"));
        else
            CHECK(result==XR_LIR_OK);
        xr_lir_module_destroy(module);
    }
}

static void cleanup_requires_a_registered_exit(void) {
    enum { EXACT, SKIPPED, FORGED_CAPTURE, WRONG_OWNER, PANIC_BEFORE_EXIT, CASE_COUNT };
    for (unsigned mode = EXACT; mode < CASE_COUNT; ++mode) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirFunction owner = test_function(module, "owner", NULL, 0, XR_LIR_UNIT), cleanup = {0};
        XrLirType scalar = XR_LIR_I64;
        XrLirFunctionSpec spec = {.name = "cleanup", .name_length = 7, .parameters = &scalar,
            .parameter_count = 1, .result = XR_LIR_UNIT, .cleanup_owner = owner.id};
        OK(xr_lir_declare_function(module, &spec, &cleanup));
        OK(xr_lir_define_function(module, cleanup));
        XrLirBlock body = test_block(module, cleanup, NULL, 0);
        OK(xr_lir_set_entry(module, cleanup, body));
        test_return(module, body, (XrLirValue){0});

        XrLirBlock registration = test_block(module, owner, NULL, 0);
        XrLirBlock active = test_block(module, owner, NULL, 0);
        OK(xr_lir_set_entry(module, owner, registration));
        XrLirValue capture = test_constant(module, registration, scalar, 17);
        XrLirValue saved = capture;
        if (mode == FORGED_CAPTURE) saved = test_constant(module, registration, scalar, 99);
        OK(xr_lir_internal_cleanup_block(module, registration, 0, cleanup.id, &capture, 1));
        test_branch(module, registration, active, NULL, 0);
        OK(xr_lir_internal_cleanup_block(module, active, cleanup.id, 0, NULL, 0));
        XrLirInstruction run = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = cleanup,
            .operands = &saved, .operand_count = 1};
        if (mode == WRONG_OWNER) {
            XrLirFunction other = test_function(module, "other", NULL, 0, XR_LIR_UNIT);
            XrLirBlock outsider = test_block(module, other, NULL, 0);
            saved = test_constant(module, outsider, scalar, 17);
            CHECK(xr_lir_emit_effect(module, outsider, &run) == XR_LIR_VERIFY_ERROR);
        } else {
            if (mode == PANIC_BEFORE_EXIT) {
                XrLirValue zero = test_constant(module, active, scalar, 0), unused = {0};
                XrLirValue operands[] = {capture, zero};
                XrLirInstruction divide = {.op = XR_LIR_DIV, .type = scalar,
                    .operands = operands, .operand_count = 2,
                    .source = {"cleanup.xr", 10, 2, 1, 2, 7}};
                OK(xr_lir_emit_value(module, active, &divide, &unused));
            }
            if (mode != SKIPPED) OK(xr_lir_emit_effect(module, active, &run));
            test_return(module, active, (XrLirValue){0});
            XrLirDiagnostic diagnostic = {0};
            XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
            if (mode == EXACT) {
                CHECK(status == XR_LIR_OK);
                XrLirModule *rebuilt = NULL;
                OK(xr_lir_rebuild(module, &rebuilt));
                OK(xr_lir_verify(rebuilt, NULL));
                xr_lir_module_destroy(rebuilt);
                XrLirModule *inlined = NULL;
                OK(xr_lir_inline(module, &inlined));
                XrLirFunction inlined_cleanup = {0};
                XrLirFunctionView view = {0};
                OK(xr_lir_function_at(inlined, cleanup.id - 1, &inlined_cleanup));
                OK(xr_lir_function_view(inlined, inlined_cleanup, &view));
                CHECK(view.cleanup_owner == owner.id);
                OK(xr_lir_verify(inlined, NULL));
                xr_lir_module_destroy(inlined);
            } else {
                CHECK(status == XR_LIR_VERIFY_ERROR && strstr(diagnostic.message, "cleanup frontier"));
            }
        }
        xr_lir_module_destroy(module);
    }
}

static void cleanup_suffix_precedes_retirement(void) {
    for (unsigned crossed = 0; crossed < 2; ++crossed) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirFunction owner = test_function(module, "owner", NULL, 0, XR_LIR_UNIT);
        XrLirFunction cleanup[2] = {{0}};
        const char *names[] = {"outer", "inner"};
        for (uint32_t n = 0; n < 2; ++n) {
            XrLirFunctionSpec spec = {.name = names[n], .name_length = 5, .result = XR_LIR_UNIT,
                .cleanup_owner = owner.id};
            OK(xr_lir_declare_function(module, &spec, &cleanup[n]));
            OK(xr_lir_define_function(module, cleanup[n]));
            XrLirBlock body = test_block(module, cleanup[n], NULL, 0);
            OK(xr_lir_set_entry(module, cleanup[n], body));
            test_return(module, body, (XrLirValue){0});
        }
        XrLirType string = XR_LIR_STRING;
        XrLirBlock outer = test_block(module, owner, NULL, 0);
        XrLirBlock inner = test_block(module, owner, &string, 1);
        XrLirBlock active = test_block(module, owner, &string, 1);
        OK(xr_lir_set_entry(module, owner, outer));
        XrLirLiteral literal = {0};
        OK(xr_lir_add_literal(module, (const uint8_t *)"owner", 5, &literal));
        XrLirValue value = {0};
        OK(xr_lir_emit_value(module, outer, &(XrLirInstruction){.op = XR_LIR_STRING_LITERAL,
            .type = string, .bits = literal.id}, &value));
        OK(xr_lir_internal_cleanup_block(module, outer, 0, cleanup[0].id, NULL, 0));
        test_branch(module, outer, inner, &value, 1);
        OK(xr_lir_block_parameter(module, inner, 0, &value));
        OK(xr_lir_internal_cleanup_block(module, inner, cleanup[0].id, cleanup[1].id, NULL, 0));
        test_branch(module, inner, active, &value, 1);
        OK(xr_lir_block_parameter(module, active, 0, &value));
        OK(xr_lir_internal_cleanup_block(module, active, cleanup[1].id, 0, NULL, 0));
        XrLirInstruction drop = {.op = XR_LIR_STRING_DROP, .type = XR_LIR_UNIT,
            .operands = &value, .operand_count = 1};
        XrLirInstruction run = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = cleanup[1]};
        OK(xr_lir_emit_effect(module, active, &run));
        if (crossed) OK(xr_lir_emit_effect(module, active, &drop));
        run.callee = cleanup[0];
        OK(xr_lir_emit_effect(module, active, &run));
        if (!crossed) OK(xr_lir_emit_effect(module, active, &drop));
        test_return(module, active, (XrLirValue){0});
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        if (crossed) {
            CHECK(status == XR_LIR_VERIFY_ERROR && strstr(diagnostic.message, "cleanup frontier"));
        } else {
            CHECK(status == XR_LIR_OK);
        }
        xr_lir_module_destroy(module);
    }
}

static void cleanup_invoke_checks_the_complete_callee(void) {
    enum { EXACT, DIVIDE_IN_CALLEE, EFFECT_ON_FAILURE, CATCH, REPLACED_CATCH, REPEATED_CATCH_ERROR, MODE_COUNT };
    for (unsigned mode = EXACT; mode < MODE_COUNT; ++mode) {
        bool catches = mode >= CATCH;
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType error = try_error_type(module, "Failure"), scalar = XR_LIR_I64;
        XrLirFunction leaf = {0}, owner = {0}, cleanup = {0};
        XrLirFunctionSpec typed = {.name = "leaf", .name_length = 4, .result = scalar, .error = error};
        OK(xr_lir_declare_function(module, &typed, &leaf));
        OK(xr_lir_define_function(module, leaf));
        XrLirBlock body = test_block(module, leaf, NULL, 0);
        OK(xr_lir_set_entry(module, leaf, body));
        XrLirValue result = test_constant(module, body, scalar, 17);
        if (mode == DIVIDE_IN_CALLEE) {
            XrLirValue args[] = {result, test_constant(module, body, scalar, 0)};
            OK(xr_lir_emit_value(module, body, &(XrLirInstruction){.op = XR_LIR_DIV, .type = scalar,
                .operands = args, .operand_count = 2, .source = {"cleanup.xr", 10, 2, 1, 2, 7}}, &result));
        }
        test_return(module, body, result);
        typed.name = "owner";
        typed.name_length = 5;
        if (catches) typed.error = 0;
        OK(xr_lir_declare_function(module, &typed, &owner));
        OK(xr_lir_define_function(module, owner));
        OK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name = "cleanup", .name_length = 7,
            .result = XR_LIR_UNIT, .cleanup_owner = owner.id}, &cleanup));
        OK(xr_lir_define_function(module, cleanup));
        body = test_block(module, cleanup, NULL, 0);
        OK(xr_lir_set_entry(module, cleanup, body));
        test_return(module, body, (XrLirValue){0});
        XrLirFunction print = {0};
        if (mode == EFFECT_ON_FAILURE)
            OK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name = "xrt_print_i64", .name_length = 13,
                .parameters = &scalar, .parameter_count = 1, .result = XR_LIR_UNIT,
                .kind = XR_LIR_FUNCTION_EXTERNAL, .extern_kind = XR_LIR_EXTERN_PRINT_I64}, &print));

        XrLirBlock registration = test_block(module, owner, NULL, 0);
        XrLirBlock call = test_block(module, owner, NULL, 0);
        XrLirBlock normal = test_block(module, owner, &scalar, 1);
        XrLirBlock failed = test_block(module, owner, &error, 1);
        XrLirBlock handler = {0};
        XrLirValue other = {0};
        XrLirType handler_types[] = {error, error};
        if (catches) handler = test_block(module, owner, handler_types, mode == REPEATED_CATCH_ERROR ? 2 : 1);
        OK(xr_lir_set_entry(module, owner, registration));
        if (mode == REPLACED_CATCH)
            OK(xr_lir_emit_value(module, registration, &(XrLirInstruction){
                .op = XR_LIR_SUM_NEW, .type = error}, &other));
        OK(xr_lir_internal_cleanup_block(module, registration, 0, cleanup.id, NULL, 0));
        test_branch(module, registration, call, NULL, 0);
        OK(xr_lir_internal_cleanup_block(module, call, cleanup.id, 0, NULL, 0));
        OK(xr_lir_set_terminator(module, call, &(XrLirInstruction){.op = XR_LIR_INVOKE, .type = scalar,
            .callee = leaf, .edges = {{.target = normal}, {.target = failed}}, .edge_count = 2}));
        XrLirBlock exits[] = {normal, failed};
        for (uint32_t e = 0; e < 2; ++e) {
            OK(xr_lir_internal_cleanup_block(module, exits[e], cleanup.id, 0, NULL, 0));
            if (e && mode == EFFECT_ON_FAILURE) {
                XrLirValue extra = test_constant(module, failed, scalar, 42);
                OK(xr_lir_emit_effect(module, failed, &(XrLirInstruction){.op = XR_LIR_CALL,
                    .type = XR_LIR_UNIT, .callee = print, .operands = &extra, .operand_count = 1}));
            }
            OK(xr_lir_emit_effect(module, exits[e], &(XrLirInstruction){.op = XR_LIR_CALL,
                .type = XR_LIR_UNIT, .callee = cleanup}));
            OK(xr_lir_block_parameter(module, exits[e], 0, &result));
            if (e && catches) {
                XrLirValue received[] = {mode == REPLACED_CATCH ? other : result, result};
                test_branch(module, exits[e], handler, received, mode == REPEATED_CATCH_ERROR ? 2 : 1);
                continue;
            }
            OK(xr_lir_set_terminator(module, exits[e], &(XrLirInstruction){
                .op = e ? XR_LIR_RETURN_ERROR : XR_LIR_RET, .type = XR_LIR_UNIT,
                .operands = &result, .operand_count = 1}));
        }
        if (catches) test_return(module, handler, test_constant(module, handler, scalar, 23));
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        if (mode == DIVIDE_IN_CALLEE)
            CHECK(status == XR_LIR_VERIFY_ERROR && strstr(diagnostic.message, "panic-free leaf"));
        else if (mode == EFFECT_ON_FAILURE || mode == REPEATED_CATCH_ERROR)
            CHECK(status == XR_LIR_VERIFY_ERROR && strstr(diagnostic.message, "cleanup frontier"));
        else if (mode == REPLACED_CATCH)
            CHECK(status == XR_LIR_VERIFY_ERROR && strstr(diagnostic.message, "dedicated normal and error successors"));
        else CHECK(status == XR_LIR_OK);
        xr_lir_module_destroy(module);
    }
}

static void cleanup_callback_cannot_assert(void) {
    enum { DIRECT_ASSERTION, NESTED_RETURN, NESTED_ASSERTION, MODE_COUNT };
    for (unsigned mode = DIRECT_ASSERTION; mode < MODE_COUNT; ++mode) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType error = try_error_type(module, "Failure");
        XrLirFunction leaf = {0};
        OK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name = "leaf", .name_length = 4,
            .result = XR_LIR_UNIT, .error = error}, &leaf));
        OK(xr_lir_define_function(module, leaf));
        XrLirBlock leaf_entry = test_block(module, leaf, NULL, 0);
        OK(xr_lir_set_entry(module, leaf, leaf_entry));
        test_return(module, leaf_entry, (XrLirValue){0});

        XrLirFunction owner = test_function(module, "owner", NULL, 0, XR_LIR_UNIT);
        XrLirFunction caller = owner, inner = {0};
        if (mode != DIRECT_ASSERTION) {
            OK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name = "callback", .name_length = 8,
                .result = XR_LIR_UNIT, .cleanup_owner = owner.id}, &caller));
            OK(xr_lir_define_function(module, caller));
            XrLirBlock register_callback = test_block(module, owner, NULL, 0);
            XrLirBlock run_callback = test_block(module, owner, NULL, 0);
            OK(xr_lir_set_entry(module, owner, register_callback));
            OK(xr_lir_internal_cleanup_block(module, register_callback, 0, caller.id, NULL, 0));
            test_branch(module, register_callback, run_callback, NULL, 0);
            OK(xr_lir_internal_cleanup_block(module, run_callback, caller.id, 0, NULL, 0));
            OK(xr_lir_emit_effect(module, run_callback, &(XrLirInstruction){.op = XR_LIR_CALL,
                .type = XR_LIR_UNIT, .callee = caller}));
            test_return(module, run_callback, (XrLirValue){0});
        }
        OK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name = "inner", .name_length = 5,
            .result = XR_LIR_UNIT, .cleanup_owner = caller.id}, &inner));
        OK(xr_lir_define_function(module, inner));
        XrLirBlock inner_entry = test_block(module, inner, NULL, 0);
        OK(xr_lir_set_entry(module, inner, inner_entry));
        test_return(module, inner_entry, (XrLirValue){0});

        XrLirBlock registration = test_block(module, caller, NULL, 0);
        XrLirBlock active = test_block(module, caller, NULL, 0);
        OK(xr_lir_set_entry(module, caller, registration));
        OK(xr_lir_internal_cleanup_block(module, registration, 0, inner.id, NULL, 0));
        test_branch(module, registration, active, NULL, 0);
        OK(xr_lir_internal_cleanup_block(module, active, inner.id, 0, NULL, 0));
        XrLirInstruction cleanup = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = inner};
        if (mode == NESTED_RETURN) {
            OK(xr_lir_emit_effect(module, active, &cleanup));
            test_return(module, active, (XrLirValue){0});
        } else {
            XrLirBlock normal = test_block(module, caller, NULL, 0);
            XrLirBlock failed = test_block(module, caller, &error, 1);
            OK(xr_lir_set_terminator(module, active, &(XrLirInstruction){.op = XR_LIR_INVOKE,
                .type = XR_LIR_UNIT, .callee = leaf,
                .edges = {{.target = normal}, {.target = failed}}, .edge_count = 2}));
            OK(xr_lir_internal_cleanup_block(module, normal, inner.id, 0, NULL, 0));
            OK(xr_lir_internal_cleanup_block(module, failed, inner.id, 0, NULL, 0));
            OK(xr_lir_emit_effect(module, normal, &cleanup));
            OK(xr_lir_emit_effect(module, failed, &cleanup));
            test_return(module, normal, (XrLirValue){0});
            XrLirValue error_value = {0};
            XrLirLiteral metadata = {0};
            OK(xr_lir_block_parameter(module, failed, 0, &error_value));
            OK(xr_lir_error_literal(module, error, &metadata));
            OK(xr_lir_set_terminator(module, failed, &(XrLirInstruction){.op = XR_LIR_PANIC_ERROR,
                .type = XR_LIR_UNIT, .operands = &error_value, .operand_count = 1, .bits = metadata.id,
                .source = {"cleanup.xr", 10, 2, 5, 2, 10}}));
        }
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        if (mode == NESTED_ASSERTION) {
            CHECK(status == XR_LIR_VERIFY_ERROR && diagnostic.function == caller.id &&
                strstr(diagnostic.message, "cleanup frontier"));
        } else CHECK(status == XR_LIR_OK);
        xr_lir_module_destroy(module);
    }
}

static void discarded_invoke_error_cannot_be_observed(void) {
    enum { UNUSED, READ_IN_SUCCESSOR, FORWARD_IN_SUCCESSOR, CASE_COUNT };
    for (unsigned mode=UNUSED; mode<CASE_COUNT; ++mode) {
        XrLirModule *m=NULL;
        OK(xr_lir_module_create(NULL,&m));
        XrLirType error=try_error_type(m,"Failure"),optional=0;
        OK(xr_lir_nullable_type(m,XR_LIR_UNIT,&optional));
        XrLirFunction leaf={0};
        OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="leaf",.name_length=4,
            .result=XR_LIR_UNIT,.error=error},&leaf));
        OK(xr_lir_define_function(m,leaf));
        XrLirBlock body=test_block(m,leaf,NULL,0);
        OK(xr_lir_set_entry(m,leaf,body));
        XrLirValue original={0};
        OK(xr_lir_emit_value(m,body,&(XrLirInstruction){.op=XR_LIR_SUM_NEW,.type=error},&original));
        OK(xr_lir_set_terminator(m,body,&(XrLirInstruction){.op=XR_LIR_RETURN_ERROR,
            .type=XR_LIR_UNIT,.operands=&original,.operand_count=1}));

        XrLirFunction caller=test_function(m,"caller",NULL,0,XR_LIR_UNIT);
        XrLirBlock call=test_block(m,caller,NULL,0),success=test_block(m,caller,NULL,0);
        XrLirBlock failed=test_block(m,caller,&error,1),absent=test_block(m,caller,NULL,0);
        XrLirType types[]={optional,error};
        XrLirBlock join=test_block(m,caller,types,mode==FORWARD_IN_SUCCESSOR ? 2u : 1u);
        OK(xr_lir_set_entry(m,caller,call));
        OK(xr_lir_set_terminator(m,call,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=XR_LIR_UNIT,
            .callee=leaf,.edges={{.target=success},{.target=failed}},.edge_count=2}));
        test_return(m,success,(XrLirValue){0});
        XrLirValue captured={0},none={0};
        OK(xr_lir_block_parameter(m,failed,0,&captured));
        test_branch(m,failed,absent,NULL,0);
        OK(xr_lir_emit_value(m,absent,&(XrLirInstruction){.op=XR_LIR_NULLABLE_NONE,.type=optional},&none));
        XrLirValue values[]={none,captured};
        test_branch(m,absent,join,values,mode==FORWARD_IN_SUCCESSOR ? 2u : 1u);
        if (mode==READ_IN_SUCCESSOR) {
            XrLirValue tag={0};
            OK(xr_lir_emit_value(m,join,&(XrLirInstruction){.op=XR_LIR_SUM_TAG,.type=XR_LIR_I64,
                .operands=&captured,.operand_count=1},&tag));
        }
        test_return(m,join,(XrLirValue){0});
        XrLirDiagnostic diagnostic={0};
        XrLirStatus status=xr_lir_module_finish(m,&diagnostic);
        if (mode==UNUSED) CHECK(status==XR_LIR_OK);
        else CHECK(status==XR_LIR_VERIFY_ERROR && strstr(diagnostic.message,"dedicated normal and error successors"));
        xr_lir_module_destroy(m);
    }
}

static void startup_error_requires_an_initialized_origin(void) {
    enum { VALID, NO_STARTUP, ORDINARY_CALLEE, CALLABLE_ROOT, WRONG_ARGUMENT,
           WRONG_ERROR, FAILED_READY, EARLY_READY, CALLEE_READY, NO_INITIALIZER_CALL, MODE_COUNT };
    for (unsigned mode=0;mode<MODE_COUNT;++mode) {
        XrLirModule *m=NULL;
        OK(xr_lir_module_create(NULL,&m));
        XrLirType error=try_error_type(m,"StartupError"),state_type=XR_LIR_STATE;
        bool stateful=mode==FAILED_READY || mode==WRONG_ARGUMENT || mode==EARLY_READY || mode==CALLEE_READY;
        if (stateful) {
            XrLirStateModule module={0};
            XrLirStateSlot slot={.payload=XR_LIR_I64,.size=8,.alignment=8};
            XrLirStateLayout layout={.modules=&module,.module_count=1,.slots=&slot,.slot_count=1,
                .payload_size=8,.payload_alignment=8};
            OK(xr_lir_set_state_layout(m,&layout));
        }
        XrLirFunction init={0};
        OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){
            .name="init",.name_length=4,.result=XR_LIR_UNIT,.error=error,
            .parameters=stateful ? &state_type : NULL,.parameter_count=stateful,
            .initializer_module=mode!=ORDINARY_CALLEE},&init));
        OK(xr_lir_define_function(m,init));
        XrLirBlock callee=test_block(m,init,NULL,0);
        OK(xr_lir_set_entry(m,init,callee));
        if (mode==CALLEE_READY) {
            XrLirValue own_state={0};
            OK(xr_lir_function_parameter(m,init,0,&own_state));
            OK(xr_lir_emit_effect(m,callee,&(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,
                .operands=&own_state,.operand_count=1}));
        }
        XrLirValue thrown={0};
        XrLirInstruction make={.op=XR_LIR_SUM_NEW,.type=error};
        OK(xr_lir_emit_value(m,callee,&make,&thrown));
        OK(xr_lir_set_terminator(m,callee,&(XrLirInstruction){.op=XR_LIR_RETURN_ERROR,
            .type=XR_LIR_UNIT,.operands=&thrown,.operand_count=1}));

        XrLirFunction root=test_function(m,"root",stateful ? &state_type : NULL,stateful,XR_LIR_UNIT);
        if (stateful) OK(xr_lir_set_state_entry(m,root));
        if (mode!=NO_STARTUP) OK(xr_lir_set_startup_entry(m,root,1));
        XrLirBlock entry=test_block(m,root,NULL,0);
        OK(xr_lir_set_entry(m,root,entry));
        if (mode==CALLABLE_ROOT) {
            XrLirFunction ordinary=test_function(m,"ordinary",NULL,0,XR_LIR_UNIT);
            XrLirBlock body=test_block(m,ordinary,NULL,0);
            OK(xr_lir_set_entry(m,ordinary,body));
            CHECK(xr_lir_emit_effect(m,body,&(XrLirInstruction){.op=XR_LIR_CALL,
                .type=XR_LIR_UNIT,.callee=root})==XR_LIR_VERIFY_ERROR);
            xr_lir_module_destroy(m);
            continue;
        }
        if (mode==NO_INITIALIZER_CALL) {
            test_return(m,entry,(XrLirValue){0});
            XrLirDiagnostic diagnostic={0};
            CHECK(xr_lir_module_finish(m,&diagnostic)==XR_LIR_VERIFY_ERROR &&
                strstr(diagnostic.message,"startup requires one invoke per typed initializer"));
            xr_lir_module_destroy(m);
            continue;
        }
        XrLirBlock normal=test_block(m,root,NULL,0),failed=test_block(m,root,&error,1);
        XrLirValue state={0};
        XrLirInstruction publication={.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,
            .operands=&state,.operand_count=1};
        if (stateful) {
            OK(xr_lir_function_parameter(m,root,0,&state));
            if (mode==EARLY_READY) {
                publication.op=XR_LIR_MODULE_READY;
                OK(xr_lir_emit_effect(m,entry,&publication));
                publication.op=XR_LIR_MODULE_BEGIN;
            }
            OK(xr_lir_emit_effect(m,entry,&publication));
        }
        XrLirValue argument=mode==WRONG_ARGUMENT ? test_constant(m,entry,XR_LIR_I64,0) : state;
        XrLirInstruction invoke={.op=XR_LIR_INVOKE,.type=XR_LIR_UNIT,.callee=init,
            .operands=stateful ? &argument : NULL,.operand_count=stateful,
            .edges={{.target=normal},{.target=failed}},.edge_count=2};
        XrLirStatus status=xr_lir_set_terminator(m,entry,&invoke);
        if (mode==NO_STARTUP || mode==ORDINARY_CALLEE || mode==WRONG_ARGUMENT) {
            CHECK(status==XR_LIR_VERIFY_ERROR);
            xr_lir_module_destroy(m);
            continue;
        }
        CHECK(status==XR_LIR_OK);
        if (stateful) {
            publication.op=XR_LIR_MODULE_READY;
            OK(xr_lir_emit_effect(m,normal,&publication));
            if (mode==FAILED_READY) OK(xr_lir_emit_effect(m,failed,&publication));
        }
        test_return(m,normal,(XrLirValue){0});
        XrLirValue received={0};
        OK(xr_lir_block_parameter(m,failed,0,&received));
        if (mode==WRONG_ERROR) OK(xr_lir_emit_value(m,failed,&make,&received));
        XrLirLiteral metadata={0};
        OK(xr_lir_error_literal(m,error,&metadata));
        OK(xr_lir_set_terminator(m,failed,&(XrLirInstruction){.op=XR_LIR_UNCAUGHT_ERROR,.type=XR_LIR_UNIT,
            .operands=&received,.operand_count=1,.bits=metadata.id}));
        XrLirDiagnostic diagnostic={0};
        status=xr_lir_module_finish(m,&diagnostic);
        if (mode==VALID) {
            CHECK(status==XR_LIR_OK);
            XrLirModule *copy=NULL;
            OK(xr_lir_rebuild(m,&copy));
            uint32_t count=0;
            XrLirFunction entry_role={0};
            OK(xr_lir_startup_entry(copy,&entry_role,&count));
            CHECK(entry_role.id==root.id && entry_role.module==copy && count==1);
            xr_lir_module_destroy(copy);
        } else {
            const char *why=mode==EARLY_READY || mode==CALLEE_READY ?
                "typed module publication is outside its startup invoke" : "dedicated normal and error successors";
            CHECK(status==XR_LIR_VERIFY_ERROR && strstr(diagnostic.message,why));
        }
        xr_lir_module_destroy(m);
    }
}

static void slice_callback_loan_conditions(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) {
        for (unsigned shape=0;shape<SLICE_CALLBACK_LOAN_CASE_COUNT;++shape) {
            SliceCallbackLoanFixture c=slice_callback_loan_fixture(elements[family],(SliceCallbackLoanCase)shape);
            bool safe=shape==SLICE_CALLBACK_LOAN_DIRECT || shape==SLICE_CALLBACK_LOAN_INDIRECT || shape==SLICE_CALLBACK_LOAN_DERIVED;
            bool alias=shape==SLICE_CALLBACK_LOAN_ALIAS || shape==SLICE_CALLBACK_LOAN_INDIRECT_ALIAS;
            XrLirStatus expected=safe?XR_LIR_OK:alias?XR_LIR_VERIFY_ERROR:XR_LIR_UNSUPPORTED;
            XrLirDiagnostic diagnostic={0};XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
            if (status!=expected) fprintf(stderr,"Slice callback loan family%zu case%u: status%u expected%u f%u b%u i%u %s\n",
                family,shape,(unsigned)status,(unsigned)expected,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.message);
            CHECK(status==expected);
            if (safe) {
                OK(xr_lir_verify(c.module,NULL));XrLirModule *copy=NULL;OK(xr_lir_rebuild(c.module,&copy));
                XrLirSliceFacts *facts=NULL;OK(xr_lir_slice_facts_create(c.module,&facts));xr_lir_module_destroy(c.module);
                XrLirSliceFactsView view={0};OK(xr_lir_slice_facts_view(facts,&view));
                CHECK(view.callback_loans_complete && view.callback_loan_count==2 &&
                    view.callback_loans[0].callback_parameter==1 && view.callback_loans[0].loan_parameter==0 &&
                    view.callback_loans[1].callback_parameter==0 && view.callback_loans[1].loan_parameter==1);
                xr_lir_slice_facts_destroy(facts);OK(xr_lir_verify(copy,NULL));xr_lir_module_destroy(copy);
            } else {
                CHECK(strstr(diagnostic.message,alias?"Array binding write":shape==SLICE_CALLBACK_LOAN_INDIRECT_SLICE?
                    "Slice use requires a closed readonly loan operation":"captured binding effect facts"));
                xr_lir_module_destroy(c.module);
            }
        }
        for (unsigned kind=0;kind<3;++kind) {
            XrLirModule *m=slice_callback_loan_ref_fixture(elements[family],kind);XrLirDiagnostic diagnostic={0};
            XrLirStatus expected=kind==1?XR_LIR_VERIFY_ERROR:XR_LIR_UNSUPPORTED;
            /* ArrayRef has no indirect call carrier; its existing availability
             * rejection precedes the conditional loan checker. */
            const char *message=kind==1?"Managed Cell handle cannot escape its activation":"captured binding effect facts";
            XrLirStatus status=xr_lir_module_finish(m,&diagnostic);
            if (status!=expected || !strstr(diagnostic.message,message))
                fprintf(stderr,"Slice callback reference family%zu kind%u status%u expected%u f%u b%u i%u %s\n",
                    family,kind,(unsigned)status,(unsigned)expected,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.message);
            CHECK(status==expected);CHECK(strstr(diagnostic.message,message));xr_lir_module_destroy(m);
        }
        XrLirModule *captured=slice_callback_loan_capture_fixture(elements[family]);XrLirDiagnostic diagnostic={0};
        CHECK(xr_lir_module_finish(captured,&diagnostic)==XR_LIR_UNSUPPORTED &&
            strstr(diagnostic.message,"captured binding effect facts"));xr_lir_module_destroy(captured);
        XrLirModule *selected=slice_callback_loan_multiroot_fixture(elements[family]);diagnostic=(XrLirDiagnostic){0};
        CHECK(xr_lir_module_finish(selected,&diagnostic)==XR_LIR_UNSUPPORTED &&
            strstr(diagnostic.message,"captured binding effect facts"));xr_lir_module_destroy(selected);
    }
}

static void slice_callable_captures(void) {
    for (unsigned shape=0;shape<SLICE_CALLABLE_CASE_COUNT;++shape) {
        XrLirModule *m=slice_callable_fixture((SliceCallableCase)shape);
        bool safe=shape==SLICE_CALLABLE_DIRECT || shape==SLICE_CALLABLE_INDIRECT ||
            shape==SLICE_CALLABLE_ALIAS || shape==SLICE_CALLABLE_INDIRECT_ARGUMENT;
        bool conflicting=shape==SLICE_CALLABLE_STATE_WRITE || shape==SLICE_CALLABLE_DIRECT_STATE_WRITE ||
            shape==SLICE_CALLABLE_INDIRECT_STATE_WRITE;
        XrLirStatus expected=safe?XR_LIR_OK:conflicting?XR_LIR_VERIFY_ERROR:XR_LIR_UNSUPPORTED;
        const char *expected_message=conflicting?"Array binding write":"captured binding effect facts";
        XrLirDiagnostic diagnostic={0};
        XrLirStatus status=xr_lir_module_finish(m,&diagnostic);
        if (status!=expected || (!safe && !strstr(diagnostic.message,expected_message)))
            fprintf(stderr,"Slice callable case %u: status %u (expected %u), f%u b%u i%u: %s; expected message: %s\n",
                shape,(unsigned)status,(unsigned)expected,diagnostic.function,diagnostic.block,
                diagnostic.instruction,diagnostic.message,safe?"none":expected_message);
        CHECK(status==expected);
        if (safe) {
            OK(xr_lir_verify(m,NULL));XrLirModule *copy=NULL;OK(xr_lir_rebuild(m,&copy));
            xr_lir_module_destroy(m);OK(xr_lir_verify(copy,NULL));xr_lir_module_destroy(copy);
        } else {
            CHECK(strstr(diagnostic.message,expected_message));
            xr_lir_module_destroy(m);
        }
    }
}

static void queries(void) {
    XrLirModule *fib = test_fib(), *sum = test_sum();
    OK(xr_lir_verify(fib, NULL));
    OK(xr_lir_verify(sum, NULL));
    CHECK(xr_lir_function_count(fib) == 3);
    XrLirFunction function = {0};
    XrLirFunctionView view;
    OK(xr_lir_function_at(fib, 0, &function));
    OK(xr_lir_function_view(fib, function, &view));
    CHECK(view.kind == XR_LIR_FUNCTION_EXTERNAL && view.extern_kind == XR_LIR_EXTERN_PRINT_I64 &&
          view.parameter_count == 1 && view.parameter_types[0] == XR_LIR_I64 &&
          view.result == XR_LIR_UNIT);
    OK(xr_lir_function_at(fib, 1, &function));
    OK(xr_lir_function_view(fib, function, &view));
    CHECK(view.entry == 1 && view.block_count == 3 && !memcmp(view.name, "fib", 3));
    XrLirBlock block = {0};
    XrLirBlockView block_view;
    XrLirInstructionView instruction;
    OK(xr_lir_block_at(fib, function, 0, &block));
    OK(xr_lir_block_view(fib, block, &block_view));
    CHECK(block_view.instruction_count == 3 && block_view.parameter_count == 0);
    OK(xr_lir_instruction_view(fib, block, 1, &instruction));
    CHECK(instruction.op == XR_LIR_ICMP && instruction.predicate == XR_LIR_LT &&
          instruction.type == XR_LIR_I1);
    XrLirType type = XR_LIR_UNIT;
    OK(xr_lir_value_type(fib, (XrLirValue) {fib, function.id, instruction.result}, &type));
    CHECK(type == XR_LIR_I1);
    CHECK(xr_lir_function_at(fib, 3, &function) == XR_LIR_INVALID_ARGUMENT && !function.id);
    CHECK(xr_lir_instruction_view(fib, block, UINT32_MAX, &instruction) ==
              XR_LIR_INVALID_ARGUMENT &&
          !instruction.op);
    CHECK(xr_lir_define_function(fib, (XrLirFunction) {fib, 2}) == XR_LIR_INVALID_STATE);
    OK(xr_lir_verify(fib, NULL));
    xr_lir_module_destroy(fib);
    xr_lir_module_destroy(sum);
}

static XrLirModule *simple(XrLirFunction *function, XrLirBlock *entry, XrLirValue *parameter) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    *function = test_function(m, "identity", &type, 1, type);
    *entry = test_block(m, *function, NULL, 0);
    OK(xr_lir_set_entry(m, *function, *entry));
    OK(xr_lir_function_parameter(m, *function, 0, parameter));
    return m;
}

static void builder_rejections(void) {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        XrLirFunction f;
        XrLirBlock entry;
        XrLirValue argument, output = {0};
        XrLirModule *m = simple(&f, &entry, &argument);
        XrLirInstruction instruction = {.op = XR_LIR_ADD, .type = XR_LIR_I64, .operand_count = 2};
        XrLirValue operands[] = {argument, argument};
        instruction.operands = operands;
        if (scenario == 0)
            instruction.type = XR_LIR_F64;
        if (scenario == 1)
            operands[1].id = 999;
        if (scenario == 2) {
            instruction.op = XR_LIR_CALL;
            instruction.callee = f;
            instruction.operand_count = 0;
        }
        if (scenario == 3) {
            instruction.op = XR_LIR_CALL_INTRINSIC;
            instruction.intrinsic = 17;
            instruction.operand_count = 0;
        }
        if (scenario == 4) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_I1;
            instruction.bits = 2;
            instruction.operand_count = 0;
        }
        if (scenario == 5) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_I8;
            instruction.bits = 256;
            instruction.operand_count = 0;
        }
        if (scenario == 6) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_F64;
            instruction.bits = UINT64_C(0x7ff0000000000001);
            instruction.operand_count = 0;
        }
        CHECK(xr_lir_emit_value(m, entry, &instruction, &output) != XR_LIR_OK && !output.id);
        CHECK(xr_lir_module_finish(m, NULL) != XR_LIR_OK);
        xr_lir_module_destroy(m);
    }
    XrLirFunction f;
    XrLirBlock entry;
    XrLirValue value;
    XrLirModule *m = simple(&f, &entry, &value);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);
    m = simple(&f, &entry, &value);
    test_return(m, entry, value);
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64};
    CHECK(xr_lir_emit_value(m, entry, &instruction, &value) == XR_LIR_INVALID_STATE && !value.id);
    xr_lir_module_destroy(m);
}

static void foreign_handles(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        XrLirFunction f, other;
        XrLirBlock entry, foreign;
        XrLirValue argument, other_argument;
        XrLirModule *m = simple(&f, &entry, &argument),
                    *different = simple(&other, &foreign, &other_argument);
        XrLirType type = XR_LIR_I64;
        XrLirFunction second = test_function(m, "second", &type, 1, type);
        XrLirValue second_argument = {0};
        OK(xr_lir_function_parameter(m, second, 0, &second_argument));
        CHECK(argument.id == second_argument.id && argument.id == other_argument.id);
        XrLirValue operands[] = {argument, scenario == 0 ? second_argument : other_argument},
                   output = {0};
        XrLirInstruction instruction = {
            .op = XR_LIR_ADD, .type = type, .operands = operands, .operand_count = 2};
        XrLirStatus status;
        if (scenario < 2)
            status = xr_lir_emit_value(m, entry, &instruction, &output);
        else if (scenario == 2) {
            instruction = (XrLirInstruction) {.op = XR_LIR_CALL,
                                              .type = type,
                                              .callee = other,
                                              .operands = &argument,
                                              .operand_count = 1};
            status = xr_lir_emit_value(m, entry, &instruction, &output);
        } else if (scenario == 3)
            status = xr_lir_set_entry(m, second, foreign);
        else {
            instruction = (XrLirInstruction) {.op = XR_LIR_BR,
                                              .type = XR_LIR_UNIT,
                                              .edges = {{.target = foreign}},
                                              .edge_count = 1};
            status = xr_lir_set_terminator(m, entry, &instruction);
        }
        CHECK(status != XR_LIR_OK && !output.id && xr_lir_module_finish(m, NULL) != XR_LIR_OK);
        xr_lir_module_destroy(m);
        xr_lir_module_destroy(different);
    }
}

static void verifier_rejections(void) {
    XrLirModule *m = test_fib();
    XrLirDiagnostic d;
    XrLirInstructionView *constant = &m->functions[1].blocks[0].instructions[0].view;
    constant->type = XR_LIR_F64;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR && d.function == 2);
    constant->type = XR_LIR_I64;
    XrLirInstructionView *comparison = &m->functions[1].blocks[0].instructions[1].view;
    uint32_t *arguments = (uint32_t *) comparison->operands, saved = arguments[0];
    arguments[0] = UINT32_MAX;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    arguments[0] = saved;
    XrLirInstructionView *call = &m->functions[1].blocks[2].instructions[2].view;
    CHECK(call->op == XR_LIR_CALL);
    call->type = XR_LIR_F64;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    call->type = XR_LIR_I64;
    --m->functions[1].blocks[0].instruction_count;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    ++m->functions[1].blocks[0].instruction_count;
    OK(xr_lir_verify(m, &d));
    xr_lir_module_destroy(m);
    m = test_sum();
    XrLirEdgeView *edge = &m->functions[1].blocks[0].instructions[2].view.edges[0];
    --edge->argument_count;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    ++edge->argument_count;
    XrLirInstructionView *add = &m->functions[1].blocks[2].instructions[0].view;
    arguments = (uint32_t *) add->operands;
    saved = arguments[0];
    arguments[0] = add->result;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR && strstr(d.message, "dominate"));
    arguments[0] = saved;
    OK(xr_lir_verify(m, &d));
    xr_lir_module_destroy(m);
}

static void definition_prevalidation_rejections(void) {
    XrLirModule *m = test_fib();
    XrLirFunctionData *f = &m->functions[1];
    XrLirBlockData *later = &f->blocks[2];
    XrLirInstructionView *comparison = &f->blocks[0].instructions[1].view;
    uint32_t *arguments = (uint32_t *)comparison->operands;
    uint32_t original = arguments[0], target = later->instructions[2].view.result;
    XrLirValueData *value = &f->values[target - 1];
    CHECK(value->block == 3 && value->instruction == 2);
    CHECK(xr_lir_internal_value_definition(f, target) == &later->instructions[2].view);
    CHECK(!xr_lir_internal_value_definition(NULL, target));
    arguments[0] = target;
    XrLirDiagnostic diagnostic = {0};
    XrLirInstructionData *storage = later->instructions;
    later->instructions = NULL;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == 2 &&
          diagnostic.block == 3 && diagnostic.instruction == 0);
    later->instructions = storage;
    uint32_t count = later->instruction_count, coordinate = value->instruction;
    CHECK(later->instruction_capacity < UINT32_MAX);
    later->instruction_count = later->instruction_capacity + 1;
    value->instruction = later->instruction_capacity;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == 2 &&
          diagnostic.block == 3 && diagnostic.instruction == 0);
    later->instruction_count = count;
    value->instruction = count;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR && diagnostic.function == 2);
    value->instruction = coordinate;
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          strstr(diagnostic.message, "dominate"));
    arguments[0] = original;
    OK(xr_lir_verify(m, &diagnostic));
    xr_lir_module_destroy(m);
    puts("cross-block definition storage rejects before lookup; restored dominance remains enforced");
}

static void graph_edges(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType boolean = XR_LIR_I1, integer = XR_LIR_I64;
    XrLirFunction f = test_function(m, "choose", &boolean, 1, integer);
    XrLirBlock join = test_block(m, f, &integer, 1), entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue condition = {0}, answer = {0};
    OK(xr_lir_function_parameter(m, f, 0, &condition));
    OK(xr_lir_block_parameter(m, join, 0, &answer));
    XrLirValue a = test_constant(m, entry, integer, 11), b = test_constant(m, entry, integer, 29);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{join, &a, 1}, {join, &b, 1}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, entry, &branch));
    test_return(m, join, answer);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView view;
    OK(xr_lir_instruction_view(m, entry, 2, &view));
    CHECK(view.edges[0].target == view.edges[1].target &&
          view.edges[0].arguments[0] != view.edges[1].arguments[0]);
    xr_lir_module_destroy(m);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        XrLirValue parameter;
        m = simple(&f, &entry, &parameter);
        if (scenario == 0)
            test_branch(m, entry, entry, NULL, 0);
        else if (scenario == 1) {
            test_return(m, entry, parameter);
            XrLirBlock unreachable = test_block(m, f, NULL, 0);
            test_return(m, unreachable, parameter);
        } else {
            XrLirBlock left = test_block(m, f, NULL, 0), right = test_block(m, f, NULL, 0),
                       merge = test_block(m, f, NULL, 0);
            condition = test_constant(m, entry, XR_LIR_I1, 1);
            branch = (XrLirInstruction) {.op = XR_LIR_CONDBR,
                                         .type = XR_LIR_UNIT,
                                         .operands = &condition,
                                         .operand_count = 1,
                                         .edges = {{.target = left}, {.target = right}},
                                         .edge_count = 2};
            OK(xr_lir_set_terminator(m, entry, &branch));
            a = test_constant(m, left, integer, 7);
            test_branch(m, left, merge, NULL, 0);
            test_branch(m, right, merge, NULL, 0);
            test_return(m, merge, a);
        }
        XrLirStatus status = xr_lir_module_finish(m, NULL);
        CHECK(status == XR_LIR_VERIFY_ERROR && !m->finished);
        xr_lir_module_destroy(m);
    }
}

static void owned_inputs_and_growth(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    char *name = xr_malloc(6), *path = xr_malloc(10);
    XrLirType *type = xr_malloc(sizeof(*type));
    CHECK(name && path && type);
    memcpy(name, "owned", 6);
    memcpy(path, "source.xr", 10);
    *type = XR_LIR_I64;
    XrLirFunctionSpec spec = {
        .name = name, .name_length = 5, .parameters = type, .parameter_count = 1, .result = *type};
    XrLirFunction f = {0};
    OK(xr_lir_declare_function(m, &spec, &f));
    memset(name, 'x', 5);
    *type = XR_LIR_F64;
    xr_free(name);
    xr_free(type);
    OK(xr_lir_define_function(m, f));
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue parameter = {0};
    OK(xr_lir_function_parameter(m, f, 0, &parameter));
    XrLirInstruction constant = {
        .op = XR_LIR_CONST, .type = XR_LIR_I64, .source = {path, 9, 2, 3, 2, 8}};
    XrLirValue result = {0};
    OK(xr_lir_emit_value(m, entry, &constant, &result));
    memset(path, 'z', 9);
    xr_free(path);
    for (uint32_t i = 0; i < 300; ++i)
        result = test_constant(m, entry, XR_LIR_I64, i);
    test_return(m, entry, parameter);
    for (uint32_t i = 0; i < 40; ++i) {
        char other_name[32];
        snprintf(other_name, sizeof(other_name), "extra_%u", i);
        XrLirFunction other = test_function(m, other_name, NULL, 0, XR_LIR_UNIT);
        XrLirBlock block = test_block(m, other, NULL, 0);
        OK(xr_lir_set_entry(m, other, block));
        test_return(m, block, (XrLirValue) {0});
    }
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirFunctionView view;
    OK(xr_lir_function_view(m, f, &view));
    CHECK(!strcmp(view.name, "owned") && view.parameter_types[0] == XR_LIR_I64 &&
          view.value_count > 255);
    XrLirInstructionView instruction;
    OK(xr_lir_instruction_view(m, entry, 0, &instruction));
    CHECK(!strcmp(instruction.source.path, "source.xr") && instruction.source.line == 2 &&
          instruction.source.column == 3);
    XrLirType actual;
    OK(xr_lir_value_type(m, result, &actual));
    CHECK(actual == XR_LIR_I64);
    xr_lir_module_destroy(m);
}

static void constants_and_swap(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "constants", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    const uint64_t bits[] = {1,
                             128,
                             32768,
                             UINT64_C(0x80000000),
                             UINT64_C(0x8000000000000000),
                             255,
                             65535,
                             UINT64_C(0xffffffff),
                             UINT64_MAX,
                             UINT64_C(0x80000000),
                             UINT64_C(0x7ff8000000000000),
                             0};
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i)
        test_constant(m, entry, (XrLirType) (XR_LIR_I1 + i), bits[i]);
    test_return(m, entry, (XrLirValue) {0});
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        XrLirInstructionView instruction;
        OK(xr_lir_instruction_view(m, entry, i, &instruction));
        CHECK(instruction.op == XR_LIR_CONST && instruction.type == (XrLirType) (XR_LIR_I1 + i) &&
              instruction.bits == bits[i]);
    }
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType scalar = XR_LIR_I64, types[] = {scalar, scalar, scalar};
    f = test_function(m, "swap", &scalar, 1, scalar);
    entry = test_block(m, f, NULL, 0);
    XrLirBlock loop = test_block(m, f, types, 3), body = test_block(m, f, NULL, 0),
               done = test_block(m, f, &scalar, 1);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue n = {0}, a = {0}, b = {0}, count = {0}, answer = {0};
    OK(xr_lir_function_parameter(m, f, 0, &n));
    OK(xr_lir_block_parameter(m, loop, 0, &a));
    OK(xr_lir_block_parameter(m, loop, 1, &b));
    OK(xr_lir_block_parameter(m, loop, 2, &count));
    OK(xr_lir_block_parameter(m, done, 0, &answer));
    XrLirValue zero = test_constant(m, entry, scalar, 0), one = test_constant(m, entry, scalar, 1);
    XrLirValue initial[] = {zero, one, n};
    test_branch(m, entry, loop, initial, 3);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, count, zero, XR_LIR_GT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{.target = body}, {done, &a, 1}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, loop, &branch));
    XrLirValue remaining = test_binary(m, body, XR_LIR_SUB, count, one, XR_LIR_PREDICATE_NONE);
    XrLirValue carried[] = {b, a, remaining};
    test_branch(m, body, loop, carried, 3);
    memset(carried, 0, sizeof(carried));
    test_return(m, done, answer);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView backedge;
    OK(xr_lir_instruction_view(m, body, 1, &backedge));
    CHECK(backedge.edges[0].arguments[0] == b.id && backedge.edges[0].arguments[1] == a.id);
    xr_lir_module_destroy(m);
}

static void first_failure_diagnostic(void) {
    XrLirFunction f;
    XrLirBlock entry;
    XrLirValue parameter, output = {0};
    XrLirModule *m = simple(&f, &entry, &parameter);
    char *path = xr_malloc(12);
    CHECK(path != NULL);
    memcpy(path, "bad-call.xr", 12);
    XrLirInstruction invalid = {
        .op = XR_LIR_CALL, .type = XR_LIR_I64, .callee = f, .source = {path, 11, 7, 5, 7, 17}};
    CHECK(xr_lir_emit_value(m, entry, &invalid, &output) == XR_LIR_VERIFY_ERROR && !output.id);
    memset(path, 'x', 11);
    xr_free(path);
    XrLirDiagnostic diagnostic;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        memset(&diagnostic, 0xa5, sizeof(diagnostic));
        CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR);
        CHECK(diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == f.id &&
              diagnostic.block == entry.id && diagnostic.instruction == 1);
        CHECK(diagnostic.source.path && !strcmp(diagnostic.source.path, "bad-call.xr") &&
              diagnostic.source.line == 7 && diagnostic.source.column == 5 &&
              diagnostic.source.end_line == 7 && diagnostic.source.end_column == 17);
        CHECK(strstr(diagnostic.message, "signature"));
    }
    invalid = (XrLirInstruction) {.op = XR_LIR_CONST, .type = XR_LIR_I64};
    CHECK(xr_lir_emit_value(m, entry, &invalid, &output) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          !strcmp(diagnostic.source.path, "bad-call.xr"));
    xr_lir_module_destroy(m);
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_module_finish(NULL, &diagnostic) == XR_LIR_INVALID_ARGUMENT &&
          diagnostic.status == XR_LIR_INVALID_ARGUMENT && !diagnostic.source.path &&
          !diagnostic.function);
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_verify(NULL, &diagnostic) == XR_LIR_INVALID_ARGUMENT &&
          diagnostic.status == XR_LIR_INVALID_ARGUMENT && !diagnostic.source.path);
    m = simple(&f, &entry, &parameter);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == f.id &&
          diagnostic.block == entry.id && strstr(diagnostic.message, "terminator"));
    xr_lir_module_destroy(m);
    m = test_fib();
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_INVALID_STATE &&
          diagnostic.status == XR_LIR_INVALID_STATE && !diagnostic.source.path);
    OK(xr_lir_verify(m, NULL));
    xr_lir_module_destroy(m);
}

static void intrinsic_and_float(void) {
    XrLirType input = XR_LIR_F64;
    XrLirIntrinsic intrinsic = {71, &input, 1, XR_LIR_F64, true, 0};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    input = XR_LIR_I1;
    intrinsic.id = 0;
    XrLirType type = XR_LIR_F64;
    XrLirFunction f = test_function(m, "float_identity", &type, 1, type);
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue parameter = {0}, converted = {0}, result = {0};
    OK(xr_lir_function_parameter(m, f, 0, &parameter));
    XrLirInstruction conversion = {
        .op = XR_LIR_FCAST, .type = XR_LIR_F64, .operands = &parameter, .operand_count = 1};
    OK(xr_lir_emit_value(m, entry, &conversion, &converted));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC,
                             .type = XR_LIR_F64,
                             .intrinsic = 71,
                             .operands = &converted,
                             .operand_count = 1};
    OK(xr_lir_emit_value(m, entry, &call, &result));
    test_return(m, entry, result);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView instruction;
    OK(xr_lir_instruction_view(m, entry, 0, &instruction));
    CHECK(instruction.op == XR_LIR_FCAST && instruction.type == XR_LIR_F64 &&
          instruction.operands[0] == parameter.id);
    OK(xr_lir_instruction_view(m, entry, 1, &instruction));
    CHECK(instruction.op == XR_LIR_CALL_INTRINSIC && instruction.intrinsic == 71 &&
          instruction.operands[0] == converted.id);
    xr_lir_module_destroy(m);
    intrinsic = (XrLirIntrinsic) {71, &type, 1, XR_LIR_F64, false, 0};
    m = NULL;
    CHECK(xr_lir_module_create(&registry, &m) == XR_LIR_UNSUPPORTED && !m);
}

static void intrinsic_query_and_transfer(void) {
    XrLirType *input = xr_malloc(sizeof(*input));
    CHECK(input);
    *input = XR_LIR_I64;
    XrLirIntrinsic entries[] = {{19, input, 1, XR_LIR_I64, true, 0}, {71, NULL, 0, XR_LIR_UNIT, true, 0}};
    XrLirRegistry registry = {entries, 2};
    XrLirModule *source = NULL, *copy = NULL;
    OK(xr_lir_module_create(&registry, &source));
    *input = XR_LIR_I1;
    xr_free(input);
    XrLirIntrinsic view = {0};
    CHECK(!xr_lir_intrinsic_count(source));
    CHECK(xr_lir_intrinsic_at(source, 0, &view) == XR_LIR_INVALID_ARGUMENT && !view.id &&
          !view.parameters);
    XrLirType type = XR_LIR_I64;
    XrLirFunction function = test_function(source, "registered", &type, 1, type);
    XrLirBlock block = test_block(source, function, NULL, 0);
    OK(xr_lir_set_entry(source, function, block));
    XrLirValue parameter = {0}, result = {0};
    OK(xr_lir_function_parameter(source, function, 0, &parameter));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC,
                             .type = type,
                             .intrinsic = 19,
                             .operands = &parameter,
                             .operand_count = 1,
                             .source = {"intrinsic-owner.xr", 18, 3, 2, 3, 6}};
    OK(xr_lir_emit_value(source, block, &call, &result));
    test_return(source, block, result);
    OK(xr_lir_module_finish(source, NULL));
    CHECK(xr_lir_intrinsic_count(source) == 2 && !xr_lir_intrinsic_count(NULL));
    OK(xr_lir_intrinsic_at(source, 0, &entries[0]));
    OK(xr_lir_intrinsic_at(source, 1, &entries[1]));
    CHECK(entries[0].id == 19 && entries[0].parameter_count == 1 &&
          entries[0].parameters[0] == type);
    CHECK(entries[1].id == 71 && !entries[1].parameters && entries[1].result == XR_LIR_UNIT &&
          entries[1].scalar_sync);
    OK(xr_lir_intrinsic_view(source, 19, &view));
    CHECK(view.parameters == entries[0].parameters && view.result == type && view.scalar_sync);
    CHECK(xr_lir_intrinsic_at(source, 2, &view) == XR_LIR_INVALID_ARGUMENT && !view.id &&
          !view.parameters);
    CHECK(xr_lir_intrinsic_view(source, 0, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_view(source, 1, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_view(NULL, 19, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_at(source, 0, NULL) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_intrinsic_view(source, 19, NULL) == XR_LIR_INVALID_ARGUMENT);
    OK(xr_lir_module_create(&registry, &copy));
    XrLirFunctionView function_view;
    XrLirInstructionView instruction_view;
    OK(xr_lir_function_view(source, function, &function_view));
    OK(xr_lir_instruction_view(source, block, 0, &instruction_view));
    XrLirFunction cloned = test_function(copy, function_view.name, function_view.parameter_types,
                                         function_view.parameter_count, function_view.result);
    XrLirBlock cloned_block = test_block(copy, cloned, NULL, 0);
    OK(xr_lir_set_entry(copy, cloned, cloned_block));
    OK(xr_lir_function_parameter(copy, cloned, 0, &parameter));
    call = (XrLirInstruction) {.op = instruction_view.op,
                               .type = instruction_view.type,
                               .intrinsic = instruction_view.intrinsic,
                               .operands = &parameter,
                               .operand_count = 1,
                               .source = instruction_view.source};
    OK(xr_lir_emit_value(copy, cloned_block, &call, &result));
    test_return(copy, cloned_block, result);
    OK(xr_lir_module_finish(copy, NULL));
    xr_lir_module_destroy(source);
    memset(entries, 0, sizeof(entries));
    OK(xr_lir_verify(copy, NULL));
    CHECK(xr_lir_intrinsic_count(copy) == 2);
    OK(xr_lir_intrinsic_view(copy, 19, &view));
    CHECK(view.parameter_count == 1 && view.parameters[0] == type && view.result == type);
    OK(xr_lir_intrinsic_view(copy, 71, &view));
    CHECK(!view.parameter_count && view.result == XR_LIR_UNIT && view.scalar_sync);
    OK(xr_lir_instruction_view(copy, cloned_block, 0, &instruction_view));
    CHECK(instruction_view.intrinsic == 19 &&
          !strcmp(instruction_view.source.path, "intrinsic-owner.xr"));
    xr_lir_module_destroy(copy);
    XrLirIntrinsic duplicate[] = {{19, &type, 1, type, true, 0}, {19, NULL, 0, XR_LIR_UNIT, true, 0}};
    registry = (XrLirRegistry) {duplicate, 2};
    copy = NULL;
    CHECK(xr_lir_module_create(&registry, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
}

static XrLirModule *literal_program(uint32_t attack) {
    XrLirModule *m = NULL;OK(xr_lir_module_create(NULL, &m));
    unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    XrLirLiteral literal = {0};OK(xr_lir_add_literal(m, bytes, sizeof(bytes), &literal));
    memset(bytes, 0xa5, sizeof(bytes));
    XrLirType parameters[] = {XR_LIR_PTR, XR_LIR_U64};
    XrLirFunction print = {0};
    XrLirFunctionSpec external = {.name="xrt_print_string", .name_length=16, .parameters=parameters,
        .parameter_count=2, .result=XR_LIR_UNIT, .kind=XR_LIR_FUNCTION_EXTERNAL,
        .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
    OK(xr_lir_declare_function(m, &external, &print));
    XrLirFunction f = test_function(m, "literal", NULL, 0, attack == 4 ? XR_LIR_PTR : XR_LIR_UNIT);
    XrLirBlock block = test_block(m, f, NULL, 0);OK(xr_lir_set_entry(m, f, block));
    XrLirValue args[2] = {{0}};
    if (attack == 1) {
        literal.id = 2;
        CHECK(xr_lir_literal_address(m, block, literal, &args[0]) == XR_LIR_INVALID_ARGUMENT && !args[0].id);
        xr_lir_module_destroy(m);return NULL;
    }
    if (attack == 2) {
        XrLirModule *other = NULL;OK(xr_lir_module_create(NULL, &other));
        XrLirLiteral foreign = {0};OK(xr_lir_add_literal(other, "x", 1, &foreign));
        CHECK(xr_lir_literal_address(m, block, foreign, &args[0]) == XR_LIR_INVALID_ARGUMENT && !args[0].id);
        xr_lir_module_destroy(other);xr_lir_module_destroy(m);return NULL;
    }
    if (attack == 3) args[0] = test_constant(m, block, XR_LIR_PTR, 0);
    else OK(xr_lir_literal_address(m, block, literal, &args[0]));
    if (attack == 4) {
        XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=args, .operand_count=1};
        CHECK(xr_lir_set_terminator(m, block, &ret) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    args[1] = test_constant(m, block, XR_LIR_U64, attack == 5 ? 3 : sizeof(bytes));
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=print,
        .operands=args, .operand_count=2};
    if (attack == 3 || attack == 5) {
        CHECK(xr_lir_emit_effect(m, block, &call) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    if (attack != 7) OK(xr_lir_emit_effect(m, block, &call));
    if (attack == 6) OK(xr_lir_emit_effect(m, block, &call));
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT};OK(xr_lir_set_terminator(m, block, &ret));
    if (attack) {
        CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    OK(xr_lir_module_finish(m, NULL));return m;
}
static void literal_ownership(void) {
    XrLirModule *m = literal_program(0), *copies[3] = {0};
    OK(xr_lir_rebuild(m, &copies[0]));OK(xr_lir_inline(m, &copies[1]));OK(xr_lir_sccp(m, &copies[2]));
    XrLirLiteral original = {0};const uint8_t *bytes = NULL;size_t length = 0;
    OK(xr_lir_literal_at(m, 0, &original));OK(xr_lir_literal_view(m, original, &bytes, &length));
    CHECK(length == 4 && xr_lir_literal_count(m) == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirLiteral owned = {0};const uint8_t *copy = NULL;size_t size = 0;
        OK(xr_lir_literal_at(copies[i], 0, &owned));OK(xr_lir_literal_view(copies[i], owned, &copy, &size));
        CHECK(copy != bytes && size == length && !memcmp(copy, bytes, size));
        CHECK(xr_lir_literal_view(copies[i], original, &copy, &size) == XR_LIR_INVALID_ARGUMENT);
    }
    xr_lir_module_destroy(m);
    const uint8_t expected[] = {'A', 0, 0xc3, 0xa9};
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirLiteral owned = {0};OK(xr_lir_literal_at(copies[i], 0, &owned));
        OK(xr_lir_literal_view(copies[i], owned, &bytes, &length));
        CHECK(length == sizeof(expected) && !memcmp(bytes, expected, sizeof(expected)));
        OK(xr_lir_verify(copies[i], NULL));xr_lir_module_destroy(copies[i]);
    }
    for (uint32_t attack = 1; attack <= 7; ++attack) CHECK(!literal_program(attack));
}

static void clock_signatures(void) {
    static const char *names[] = {"xrt_clock_realtime_nanos", "xrt_clock_process_cpu_nanos",
        "xrt_clock_monotonic_nanos"};
    for (uint32_t kind=0; kind<3; ++kind) {
        XrLirModule *m = NULL; XrLirFunction f = {0};
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunctionSpec spec = {.name=names[kind], .name_length=strlen(names[kind]),
            .result=XR_LIR_I64, .kind=XR_LIR_FUNCTION_EXTERNAL,
            .extern_kind=(XrLirExternKind)(XR_LIR_EXTERN_CLOCK_REALTIME + kind)};
        OK(xr_lir_declare_function(m, &spec, &f));
        OK(xr_lir_module_finish(m, NULL));
        OK(xr_lir_verify(m, NULL)); xr_lir_module_destroy(m); m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        spec.result = XR_LIR_UNIT;
        CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
        xr_lir_module_destroy(m);
    }
}
static void offset_signature(void) {
    const XrLirType argument = XR_LIR_I64, wrong = XR_LIR_U64;
    XrLirModule *m = NULL; XrLirFunction f = {0};
    XrLirFunctionSpec spec = {.name="xrt_clock_utc_offset_at", .name_length=23,
        .result=XR_LIR_I64, .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=XR_LIR_EXTERN_UTC_OFFSET_AT,
        .parameters=&argument, .parameter_count=1};
    OK(xr_lir_module_create(NULL, &m)); OK(xr_lir_declare_function(m, &spec, &f));
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL)); xr_lir_module_destroy(m); m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    spec.parameter_count = 0;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    spec.parameter_count = 1; spec.parameters = &wrong;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    spec.parameters = &argument; spec.result = XR_LIR_UNIT;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
}
static void string_phi_owners(void) {
    for (unsigned attack = 0; attack < 7; ++attack) {
        XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_I1, XR_LIR_STRING};
        XrLirFunction f = test_function(m, "choose", types, 2, XR_LIR_STRING);
        XrLirBlock entry = test_block(m,f,NULL,0), left = test_block(m,f,NULL,0), right = test_block(m,f,NULL,0);
        XrLirType outputs[] = {XR_LIR_STRING,XR_LIR_STRING};
        XrLirBlock join = test_block(m,f,outputs,attack == 2 ? 2u : 1u);
        OK(xr_lir_set_entry(m,f,entry));
        XrLirValue condition,borrowed; OK(xr_lir_function_parameter(m,f,0,&condition));
        OK(xr_lir_function_parameter(m,f,1,&borrowed));
        XrLirInstruction branch = {.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edges={{left,NULL,0},{right,NULL,0}},.edge_count=2};
        OK(xr_lir_set_terminator(m,entry,&branch));
        XrLirLiteral literal = {0}; OK(xr_lir_add_literal(m,"x",1,&literal));
        XrLirValue values[2]; XrLirBlock arms[] = {left,right};
        for (unsigned arm = 0; arm < 2; ++arm) {
            XrLirInstruction string = {.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
            OK(xr_lir_emit_value(m,arms[arm],&string,&values[arm]));
            if (attack == 4 && !arm) { XrLirValue leaked; OK(xr_lir_emit_value(m,left,&string,&leaked)); }
            if (attack == 1 && !arm) {
                XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&values[arm],.operand_count=1};
                OK(xr_lir_emit_effect(m,left,&drop)); values[arm]=borrowed;
            }
            XrLirValue arguments[] = {values[arm],values[arm]};
            if (attack == 6 && arm) test_branch(m,right,right,NULL,0);
            else test_branch(m,arms[arm],join,arguments,attack == 2 ? 2u : 1u);
        }
        XrLirValue result; OK(xr_lir_block_parameter(m,join,0,&result));
        if (attack == 2) {
            XrLirValue second; OK(xr_lir_block_parameter(m,join,1,&second));
            XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&second,.operand_count=1};
            OK(xr_lir_emit_effect(m,join,&drop));
        }
        if (attack == 3 || attack == 5) {
            XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1};
            OK(xr_lir_emit_effect(m,join,&drop));
            if (attack == 3) {
                XrLirValue retained; XrLirInstruction retain = {.op=XR_LIR_STRING_RETAIN,.type=XR_LIR_STRING,.operands=&result,.operand_count=1};
                OK(xr_lir_emit_value(m,join,&retain,&retained)); result=retained;
            } else OK(xr_lir_emit_effect(m,join,&drop));
        }
        test_return(m,join,result);
        CHECK(xr_lir_verify(m,NULL) == (attack ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (!attack) OK(xr_lir_module_finish(m,NULL));
        xr_lir_module_destroy(m);
    }
}
static void string_loop_owners(void) {
    for(unsigned attack=0;attack<4;++attack) {
        XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
        XrLirType parameter=XR_LIR_STRING;
        XrLirFunction f=test_function(m,"temporary_loop",attack==3?&parameter:NULL,attack==3?1u:0u,XR_LIR_I64);
        XrLirBlock entry=test_block(m,f,NULL,0),header=test_block(m,f,(XrLirType[]){XR_LIR_I64},1),
            body=test_block(m,f,NULL,0),done=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,entry));
        XrLirValue zero=test_constant(m,entry,XR_LIR_I64,0);test_branch(m,entry,header,&zero,1);
        XrLirValue iteration;OK(xr_lir_block_parameter(m,header,0,&iteration));
        XrLirValue limit=test_constant(m,header,XR_LIR_I64,3),condition=test_binary(m,header,XR_LIR_ICMP,iteration,limit,XR_LIR_LT);
        XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edges={{body,NULL,0},{done,NULL,0}},.edge_count=2};OK(xr_lir_set_terminator(m,header,&branch));
        XrLirLiteral literal={0};OK(xr_lir_add_literal(m,"temporary",9,&literal));XrLirValue text;
        XrLirInstruction string={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,body,&string,&text));
        XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
        if(attack!=1)OK(xr_lir_emit_effect(m,body,&drop));
        if(attack==2) {
            XrLirValue after;XrLirInstruction retain={.op=XR_LIR_STRING_RETAIN,.type=XR_LIR_STRING,.operands=&text,.operand_count=1};
            OK(xr_lir_emit_value(m,body,&retain,&after));drop.operands=&after;OK(xr_lir_emit_effect(m,body,&drop));
        }
        XrLirValue one=test_constant(m,body,XR_LIR_I64,1),next=test_binary(m,body,XR_LIR_ADD,iteration,one,XR_LIR_PREDICATE_NONE);
        test_branch(m,body,header,&next,1);test_return(m,done,iteration);
        bool invalid = attack == 1 || attack == 2;
        CHECK(xr_lir_verify(m,NULL)==(invalid?XR_LIR_VERIFY_ERROR:XR_LIR_OK));
        if(!invalid)OK(xr_lir_module_finish(m,NULL));
        xr_lir_module_destroy(m);
    }
}
static void string_sequential_owners(void) {
    for(unsigned attack=0;attack<5;++attack) {
        XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirType flag=XR_LIR_I1,owned=XR_LIR_STRING;
        XrLirFunction f=test_function(m,"three_owned_branches",&flag,1,XR_LIR_STRING);
        XrLirBlock current=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,current));
        XrLirValue condition;OK(xr_lir_function_parameter(m,f,0,&condition));
        XrLirLiteral literal={0};OK(xr_lir_add_literal(m,"A",1,&literal));XrLirValue value;
        XrLirInstruction string={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};OK(xr_lir_emit_value(m,current,&string,&value));
        if(attack==4) {
            XrLirValue cell;XrLirInstruction create={.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&value,.operand_count=1};
            OK(xr_lir_emit_value(m,current,&create,&cell));XrLirInstruction drop={.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
            OK(xr_lir_emit_effect(m,current,&drop));
        }
        for(unsigned stage=0;stage<3;++stage) {
            XrLirType pair[]={owned,owned};bool duplicate=attack==2&&stage==1;
            XrLirBlock arms[]={test_block(m,f,pair,duplicate?2u:1u),test_block(m,f,&owned,1)},join=test_block(m,f,&owned,1);
            XrLirValue arguments[]={value,value};
            if(attack==3&&stage==1){XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};OK(xr_lir_emit_effect(m,current,&drop));}
            XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
                .edges={{arms[0],arguments,duplicate?2u:1u},{arms[1],arguments,1}},.edge_count=2};OK(xr_lir_set_terminator(m,current,&branch));
            for(unsigned arm=0;arm<2;++arm) {
                XrLirValue incoming;OK(xr_lir_block_parameter(m,arms[arm],0,&incoming));
                if(duplicate&&!arm){XrLirValue second;OK(xr_lir_block_parameter(m,arms[arm],1,&second));XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&second,.operand_count=1};OK(xr_lir_emit_effect(m,arms[arm],&drop));}
                if(attack==1&&stage==1&&!arm){XrLirValue leaked;OK(xr_lir_emit_value(m,arms[arm],&string,&leaked));}
                test_branch(m,arms[arm],join,&incoming,1);
            }
            current=join;OK(xr_lir_block_parameter(m,join,0,&value));
        }
        test_return(m,current,value);CHECK(xr_lir_verify(m,NULL)==(attack?XR_LIR_VERIFY_ERROR:XR_LIR_OK));
        if(!attack)OK(xr_lir_module_finish(m,NULL));
        xr_lir_module_destroy(m);
    }
}
static void string_length_types(void) {
    for(unsigned attack=0;attack<3;++attack) {
        XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
        XrLirType type=attack==1?XR_LIR_I64:XR_LIR_STRING;
        XrLirFunction f=test_function(m,"borrowed_length",&type,1,XR_LIR_I64);
        XrLirBlock block=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,block));
        XrLirValue input,result={0};OK(xr_lir_function_parameter(m,f,0,&input));
        XrLirInstruction length={.op=XR_LIR_STRING_LEN,.type=attack==2?XR_LIR_STRING:XR_LIR_I64,
            .operands=&input,.operand_count=1};
        XrLirStatus status=xr_lir_emit_value(m,block,&length,&result);
        if(attack) CHECK(status!=XR_LIR_OK&&!result.id);
        else {OK(status);test_return(m,block,result);OK(xr_lir_verify(m,NULL));OK(xr_lir_module_finish(m,NULL));}
        xr_lir_module_destroy(m);
    }
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral literal={0};OK(xr_lir_add_literal(m,"kept",4,&literal));
    XrLirFunction f=test_function(m,"released_length",NULL,0,XR_LIR_I64);
    XrLirBlock block=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,block));
    XrLirValue text,result;
    XrLirInstruction create={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,block,&create,&text));
    XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,block,&drop));
    XrLirInstruction length={.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,block,&length,&result));test_return(m,block,result);
    CHECK(xr_lir_verify(m,NULL)==XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);
}
static XrLirValue frame_string_cell(XrLirModule *m, XrLirBlock block) {
    XrLirLiteral literal = {0}; XrLirValue text, cell;
    OK(xr_lir_add_literal(m, "frame", 5, &literal));
    XrLirInstruction create = {.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,block,&create,&text));
    create=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,block,&create,&cell));
    XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,block,&drop));
    return cell;
}
static void frame_cell_drop(XrLirModule *m, XrLirBlock block, XrLirValue cell) {
    XrLirInstruction drop={.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_effect(m,block,&drop));
}
static void frame_cell_read(XrLirModule *m, XrLirBlock block, XrLirValue cell) {
    XrLirValue text;
    XrLirInstruction read={.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_value(m,block,&read,&text));
    XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,block,&drop));
}
static void frame_string_cells(void) {
    for (unsigned attack=0; attack<8; ++attack) {
        XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
        XrLirType flag=XR_LIR_I1,cell_type=XR_LIR_STRING_CELL;
        XrLirFunction f=test_function(m,"frame_returns",attack==5?&cell_type:&flag,1,XR_LIR_I64);
        XrLirBlock entry=test_block(m,f,NULL,0),yes=test_block(m,f,NULL,0),no=test_block(m,f,NULL,0);
        OK(xr_lir_set_entry(m,f,entry));
        XrLirValue cell,condition;
        if (attack==5) {
            OK(xr_lir_function_parameter(m,f,0,&cell));
            condition=test_constant(m,entry,XR_LIR_I1,1);
        } else {
            cell=frame_string_cell(m,entry);
            OK(xr_lir_function_parameter(m,f,0,&condition));
        }
        if(attack==6) {
            XrLirValue text,new_cell={0};
            XrLirInstruction read={.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&cell,.operand_count=1};
            OK(xr_lir_emit_value(m,yes,&read,&text));
            XrLirInstruction create={.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
            CHECK(xr_lir_emit_value(m,yes,&create,&new_cell)!=XR_LIR_OK && !new_cell.id);
            xr_lir_module_destroy(m);continue;
        }
        if (attack==3) frame_cell_drop(m,entry,cell);
        XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edges={{yes,NULL,0},{no,NULL,0}},.edge_count=2};
        OK(xr_lir_set_terminator(m,entry,&branch));
        for (unsigned arm=0; arm<2; ++arm) {
            XrLirBlock block=arm?no:yes;
            if (attack!=6 || !arm) frame_cell_read(m,block,cell);
            if (!(attack==1 && arm)) frame_cell_drop(m,block,cell);
            if (attack==2 && !arm) frame_cell_drop(m,block,cell);
            if (attack==4 && !arm) frame_cell_read(m,block,cell);
            if (attack==7 && !arm) test_branch(m,block,entry,NULL,0);
            else test_return(m,block,test_constant(m,block,XR_LIR_I64,arm));
        }
        CHECK(xr_lir_verify(m,NULL)==(attack?XR_LIR_VERIFY_ERROR:XR_LIR_OK));
        if (!attack) {
            OK(xr_lir_module_finish(m,NULL));
            XrLirModule *copy=NULL;OK(xr_lir_rebuild(m,&copy));OK(xr_lir_verify(copy,NULL));xr_lir_module_destroy(copy);
        }
        xr_lir_module_destroy(m);
    }
    // A single block may still release early when the handle is never used again.
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction f=test_function(m,"early_release",NULL,0,XR_LIR_I64);
    XrLirBlock entry=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,entry));
    frame_cell_drop(m,entry,frame_string_cell(m,entry));
    test_return(m,entry,test_constant(m,entry,XR_LIR_I64,7));OK(xr_lir_module_finish(m,NULL));
    xr_lir_module_destroy(m);
    m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirFunction escaped={0};
    XrLirFunctionSpec spec={.name="escaped_cell",.name_length=12,.result=XR_LIR_STRING_CELL};
    CHECK(xr_lir_declare_function(m,&spec,&escaped)==XR_LIR_INVALID_ARGUMENT && !escaped.id);
    xr_lir_module_destroy(m);
    m=NULL;OK(xr_lir_module_create(NULL,&m));
    f=test_function(m,"cell_edge",NULL,0,XR_LIR_I64);
    entry=test_block(m,f,NULL,0);XrLirType type=XR_LIR_STRING_CELL;
    XrLirBlock done=test_block(m,f,&type,1);OK(xr_lir_set_entry(m,f,entry));
    XrLirValue cell=frame_string_cell(m,entry);
    test_branch(m,entry,done,&cell,1);frame_cell_drop(m,done,cell);
    test_return(m,done,test_constant(m,done,XR_LIR_I64,7));
    CHECK(xr_lir_verify(m,NULL)==XR_LIR_VERIFY_ERROR);xr_lir_module_destroy(m);
}

#include "lir_array_block_cell_cases.h"
#include "lir_array_cell_cfg_cases.h"
#include "lir_array_loop_owner_cases.h"
#include "lir_type_pool_cases.h"
#include "lir_fixed_array_op_cases.h"
#include "lir_nullable_type_cases.h"
#include "lir_display_cases.h"
#include "lir_callable_type_cases.h"
#include "lir_owned_cell_type_cases.h"
#include "lir_string_cases.h"
#include "lir_range_cases.h"

int main(void) {
    range_catalog_receivers();
    callable_type_owners();
    callable_type_rejections();
    nullable_array_callable_shapes();
    callable_parameter_contracts();
    callable_reference_shapes();
    string_catalog_owners();
    string_reference_lifetime();
    nullable_type_owners();
    nullable_type_rejections();
    display_catalog_owners();
    nullable_instruction_rejections();
    owned_cell_type_receivers();
    owned_cell_type_rejections();
    fixed_array_operations();
    fixed_array_operation_rejections();
    type_pool_queries_and_receivers();
    type_pool_rejections();
    type_pool_calls_and_state();
    array_block_cells();
    array_cell_cfg();
    array_loop_owners();
    frame_string_cells();
    string_length_types();
    string_sequential_owners();
    string_loop_owners();
    string_phi_owners();
    offset_signature();
    clock_signatures();
    queries();
    literal_ownership();
    builder_rejections();
    foreign_handles();
    verifier_rejections();
    typed_error_cannot_be_erased();
    invoke_rejects_forged_error();
    invoke_must_forward_its_received_error();
    cleanup_requires_a_registered_exit();
    cleanup_suffix_precedes_retirement();
    cleanup_invoke_checks_the_complete_callee();
    cleanup_callback_cannot_assert();
    startup_error_requires_an_initialized_origin();
    discarded_invoke_error_cannot_be_observed();
    definition_prevalidation_rejections();
    graph_edges();
    owned_inputs_and_growth();
    constants_and_swap();
    intrinsic_and_float();
    intrinsic_query_and_transfer();
    first_failure_diagnostic();
    slice_callable_captures();
    slice_callback_loan_conditions();
    puts("scalar LIR construction, queries, ownership and rejection tests passed");
    return 0;
}
