/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xaot_callable_allocations.c - Callable analysis allocation failure cleanup
 */

#include "aot/xaot_callable.h"
#include "aot/xaot_boundary.h"
#include "../program/xr_program_allocation_probe.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static size_t attempts, fail_at, live_count;
static void *live[128];

static void *track(void *pointer) {
    REQUIRE(pointer != NULL);
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i])
            continue;
        live[i] = pointer;
        live_count++;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at)
        return NULL;
    if (!pointer)
        return track(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        void *grown = xr_program_test_system_realloc(pointer, size);
        REQUIRE(grown != NULL);
        live[i] = grown;
        return grown;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        live[i] = NULL;
        live_count--;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

static void clear_callable_output(XaotBundle *bundle) {
    xr_program_test_free(bundle->callable_invoke_plans);
    xr_program_test_free(bundle->callable_target_cases);
    bundle->callable_invoke_plans = NULL;
    bundle->callable_target_cases = NULL;
    bundle->ncallable_invoke_plans = bundle->callable_invoke_plan_cap = 0;
    bundle->ncallable_target_cases = bundle->callable_target_case_cap = 0;
}

static void test_imported_constructor_binding(void) {
    XiFunc constructor = {.name = "constructor"};
    XiFunc *children[] = {&constructor};
    XiFunc library_root = {.children = children, .nchildren = 1};
    XiClassMethod method = {.name = "constructor", .is_constructor = true};
    uint16_t child_index = 0;
    XiClassData cls = {.class_name = "Storage", .methods = &method,
                       .child_idx = &child_index, .nmethod = 1, .ninst = 1};
    XiModuleExport export = {.name = "Storage", .class_data = &cls};
    XiModule library = {.path = "library.xr", .name = "library", .init = &library_root,
                        .exports = &export, .nexports = 1};
    XiImportRef import = {.module_path = "library", .member_name = "Storage",
                          .resolved_mod_index = -1, .resolved_shared_slot = -1,
                          .resolved_export_slot = -1};
    XiImportRef *imports[] = {&import};
    XiValue read = {.op = XI_GET_SHARED, .id = 0, .aux_int = 0};
    XiValue *args[] = {&read};
    XiValue call = {.op = XI_CALL, .id = 1, .args = args, .nargs = 1};
    XiValue *values[] = {&read, &call};
    XiBlock block = {.values = values, .nvalues = 2};
    XiBlock *blocks[] = {&block};
    XiFunc caller = {.blocks = blocks, .nblocks = 1};
    XiModule entry = {.init = &caller, .slot_imports = imports, .nslots = 1};
    caller.module = &entry;
    XiModule *modules[] = {&library, &entry};
    XaotFuncPlan function = {.func = &caller, .module_index = 1};
    XaotBundle bundle = {.modules = modules, .nmodules = 2,
                         .func_plans = &function, .nfunc_plans = 1};
    XaotBoundaryCallTargets targets[2];
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, &caller, targets, 2));
    REQUIRE(targets[1].direct == &constructor && targets[1].parameter_target == &constructor &&
            targets[1].first_arg == 1 && targets[1].first_param == 1);
    import.member_name = "Missing";
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, &caller, targets, 2));
    REQUIRE(targets[1].direct == NULL && targets[1].parameter_target == NULL);
    import.member_name = "Storage";
    import.module_path = "other";
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, &caller, targets, 2));
    REQUIRE(targets[1].direct == NULL && targets[1].parameter_target == NULL);
    import.module_path = "library";
    read.op = XI_IMPORT_REF;
    read.aux = &import;
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, &caller, targets, 2));
    REQUIRE(targets[1].direct == &constructor && targets[1].first_param == 1);
}

int main(void) {
    test_imported_constructor_binding();
    XrType integer = {.kind = XR_KIND_INT, .id = 1, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType callable = {
        .kind = XR_KIND_FUNCTION, .id = 2, .frozen = true, .function = {.return_type = &integer}};
    XiFunc *root = xi_func_new("root", &integer);
    XiFunc *callee = xi_func_new("callee", &integer);
    REQUIRE(root && callee);
    XiBlock *entry = xi_block_new(root);
    XiBlock *callee_entry = xi_block_new(callee);
    REQUIRE(entry && callee_entry);
    XiValue *answer = xi_const_int(callee, callee_entry, 42, &integer);
    REQUIRE(answer);
    xi_block_set_return(callee_entry, answer);
    XiValue *closure = xi_value_new(root, entry, XI_CLOSURE_NEW, &callable, 0);
    XiValue *call = xi_value_new(root, entry, XI_CALL, &integer, 1);
    REQUIRE(closure && call);
    closure->aux = callee;
    call->args[0] = closure;
    xi_block_set_return(entry, call);
    XiModule module = {.init = root};
    XiModule *modules[] = {&module};
    XaotFuncPlan functions[] = {{.func = root, .module_index = 0},
                                {.func = callee, .module_index = 0}};
    XaotBundle bundle = {
        .modules = modules, .nmodules = 1, .func_plans = functions, .nfunc_plans = 2};
    XaotBoundaryCallTargets targets[8];
    REQUIRE(root->next_value_id <= XR_COUNTOF(targets));
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, root, targets, XR_COUNTOF(targets)));
    REQUIRE(targets[call->id].direct == callee && targets[call->id].parameter_target == callee &&
            targets[call->id].first_arg == 1 && targets[call->id].first_param == 0);
    closure->aux = root;
    REQUIRE(xaot_boundary_resolve_function_calls(&bundle, root, targets, XR_COUNTOF(targets)));
    REQUIRE(targets[call->id].direct == root && targets[call->id].parameter_target == root);
    closure->aux = callee;
    uint32_t saved_call_id = call->id;
    call->id = XR_COUNTOF(targets);
    REQUIRE(!xaot_boundary_resolve_function_calls(&bundle, root, targets, XR_COUNTOF(targets)));
    REQUIRE(targets[saved_call_id].direct == NULL && targets[saved_call_id].parameter_target == NULL);
    call->id = saved_call_id;
    XaotBoundaryCallTargets later_targets[8];
    XaotBoundaryFunctionCalls batches[] = {
        {root, targets, XR_COUNTOF(targets)},
        {root, later_targets, saved_call_id},
    };
    REQUIRE(!xaot_boundary_resolve_call_batches(&bundle, batches, XR_COUNTOF(batches)));
    REQUIRE(targets[saved_call_id].direct == NULL && targets[saved_call_id].parameter_target == NULL);
    batches[1].target_count = XR_COUNTOF(later_targets);
    REQUIRE(xaot_boundary_resolve_call_batches(&bundle, batches, XR_COUNTOF(batches)));
    REQUIRE(targets[saved_call_id].direct == callee && later_targets[saved_call_id].direct == callee);
    batches[1].function = NULL;
    REQUIRE(!xaot_boundary_resolve_call_batches(&bundle, batches, XR_COUNTOF(batches)));
    REQUIRE(targets[saved_call_id].direct == NULL && later_targets[saved_call_id].direct == NULL);
    REQUIRE(xaot_callable_plans_build(&bundle));
    size_t build_attempts = attempts;
    REQUIRE(bundle.has_callable_reachability && functions[0].reachable && functions[1].reachable);
    clear_callable_output(&bundle);
    REQUIRE(live_count == 0 && build_attempts > 0);
    for (size_t failure = 1; failure <= build_attempts; failure++) {
        attempts = 0;
        fail_at = failure;
        REQUIRE(!xaot_callable_plans_build(&bundle));
        REQUIRE(attempts >= failure && !bundle.has_callable_reachability);
        clear_callable_output(&bundle);
        REQUIRE(live_count == 0);
    }
    attempts = 0;
    fail_at = 0;
    REQUIRE(xaot_callable_plans_build(&bundle));
    REQUIRE(attempts == build_attempts);
    clear_callable_output(&bundle);
    REQUIRE(live_count == 0);
    xi_func_free(callee);
    xi_func_free(root);
    printf("Callable allocation failure cleanup: %zu points passed\n", build_attempts);
    return 0;
}
