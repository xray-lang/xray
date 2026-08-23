/*
 * test_xtp_format.c - Exact typed TargetPlan artifact contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/format/xr_artifact_kind.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/xr_runtime_artifact_authority_internal.h"
#include "../../../src/shared/xr_assertion_plan.h"
#include "../../../include/xray_target_plan_load.h"
#include "../../../include/xray_runtime_generation.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/os/os_thread.h"
#include "../../../src/runtime/value/xtype.h"
#include "target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct XtpFixture {
    XrSemanticPlan *semantic;
    XrSemanticPlan *dependency;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
} XtpFixture;

static const char k_xtp_fixture_identity[] =
    "memory-module-v1:id=21:xtp-format-fixture-v1";

static bool xtp_fixture_build(XiFunc *root, XrSemanticPlan **plan, char *error,
                              size_t error_size) {
    XiModule fixture = {
        .identity = (char *) k_xtp_fixture_identity,
        .path = "xtp-format-fixture.xr",
        .name = "xtp_format_fixture",
        .init = root,
    };
    XiModule *saved_module = root ? root->module : NULL;
    bool installed_module = root && !root->module;
    bool installed_identity = root && root->module && !root->module->identity;
    if (installed_module)
        root->module = &fixture;
    else if (installed_identity)
        root->module->identity = (char *) k_xtp_fixture_identity;
    bool built = xr_semantic_plan_build(root, plan, error, error_size);
    if (installed_identity)
        root->module->identity = NULL;
    if (installed_module)
        root->module = saved_module;
    return built;
}

#define xr_semantic_plan_build xtp_fixture_build

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {
    .kind = XR_KIND_BOOL,
    .id = 8,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 3,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 2,
    .frozen = true,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrType stub_channel = {
    .kind = XR_KIND_CHANNEL,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_module_namespace = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 5,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_stringbuilder = {
    .kind = XR_KIND_INSTANCE,
    .id = 6,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "StringBuilder"},
};
static XrType stub_string = {
    .kind = XR_KIND_STRING,
    .id = 7,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static XrSemanticPlan *build_assertion_artifact_semantic_plan(void) {
    XiFunc *function =
        xi_func_new("xtp_assertion_artifact", &stub_unit);
    XiBlock *entry = function ? xi_block_new(function) : NULL;
    XiValue *condition = entry
                             ? xi_const_bool(function, entry, true,
                                             &stub_bool)
                             : NULL;
    XiValue *assertion = entry
                             ? xi_value_new(function, entry, XI_ASSERTION,
                                            &stub_unit, 1)
                             : NULL;
    REQUIRE(function && entry && condition && assertion);
    assertion->args[0] = condition;
    assertion->source_span = (XiSourceSpan) {5, 1, 5, 13};
    XrAssertionPlan assertion_plan;
    XrLocation source = {"xtp-format-fixture.xr", 5, 1, 5, 13};
    REQUIRE(xr_assertion_plan_build(
                XR_CORE_BUILTIN_ASSERT, 1, source,
                XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL,
                XR_ASSERTION_CAPABILITY_NONE, &assertion_plan) ==
            XR_ASSERTION_PLAN_OK);
    REQUIRE(xi_value_set_assertion_plan(function, assertion,
                                        &assertion_plan));
    xi_block_set_return(entry, assertion);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, diagnostic,
                                   sizeof(diagnostic)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_string_concat_release_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_string_concat_release", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_str(function, entry, "left", &stub_string);
    XiValue *right = xi_const_str(function, entry, "right", &stub_string);
    XiValue *concat =
        xi_value_new(function, entry, XI_STR_CONCAT, &stub_string, 2);
    REQUIRE(left != NULL && right != NULL && concat != NULL);
    concat->args[0] = left;
    concat->args[1] = right;
    XiValue *release =
        xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(release != NULL && result != NULL);
    release->args[0] = concat;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error,
                                   sizeof(error)));
    REQUIRE(semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_string_coroutine_lifecycle_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_string_coroutine_lifecycle", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    XiValue *left = xi_const_str(function, entry, "xtp", &stub_string);
    XiValue *right = xi_const_str(function, entry, "-lifecycle", &stub_string);
    XiValue *text =
        xi_value_new(function, entry, XI_STR_CONCAT, &stub_string, 2);
    XiValue *yield = xi_value_new(function, entry, XI_YIELD, &stub_unit, 0);
    XiValue *length = xi_value_new(function, entry, XI_LEN, &stub_int, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(left && right && text && yield && length && release);
    text->args[0] = left;
    text->args[1] = right;
    length->args[0] = text;
    release->args[0] = text;
    xi_block_set_return(entry, length);
    function->stage = XI_STAGE_SEMANTIC_LOWERED;
    function->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(function, NULL));
    REQUIRE(function->coro_plan && function->coro_plan->nstates == 1 &&
            function->coro_plan->points[0].nroots == 1 &&
            function->coro_plan->points[0].ndrops == 1);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error,
                                   sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_stringbuilder_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_stringbuilder", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder =
        xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_stringbuilder, 0);
    REQUIRE(builder != NULL);
    builder->aux = (void *) "StringBuilder";
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = builder;
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static int source_export_call_suspendability(void *ud, const XiFunc *current,
                                             const XiValue *call) {
    (void) ud;
    (void) current;
    return call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? 1
               : -1;
}

static const XiFunc *source_export_resolve_method(void *ud,
                                                  const XiFunc *current,
                                                  const XiValue *call) {
    (void) current;
    return ud && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? (const XiFunc *) ud
               : NULL;
}

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    REQUIRE(xi_const_int(function, entry, 7, &stub_int) != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_exported_semantic_plan(void) {
    XiFunc *root = xi_func_new("xtp_export_init", &stub_unit);
    XiFunc *function = xi_func_new("xtp_probe", &stub_int);
    REQUIRE(root && function);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *function_entry = xi_block_new(function);
    REQUIRE(root_entry && function_entry);
    root_entry->sealed = function_entry->sealed = true;
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children);
    root->children[0] = function;
    root->nchildren = root->children_cap = 1;
    function->parent_func = root;
    XiValue *closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW,
                                    &stub_function, 0);
    XiValue *store = xi_value_new(root, root_entry, XI_SET_SHARED,
                                  &stub_unit, 1);
    REQUIRE(closure && store);
    closure->aux = function;
    store->args[0] = closure;
    store->aux_int = 0;
    root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *result = xi_const_int(function, function_entry, 42, &stub_int);
    REQUIRE(result);
    xi_block_set_return(function_entry, result);
    root->stage = function->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = function->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    root->stage = function->stage = XI_STAGE_OPTIMIZED;
    XiModule *module = xi_module_new("fixtures/runtime_export.xr",
                                    "runtime_export", root);
    REQUIRE(module);
    REQUIRE(xi_module_set_identity(
        module, "memory-module-v1:id=29:xtp-runtime-export-fixture-v1"));
    root->module = module;
    module->nslots = 1;
    module->nexports = 1;
    module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*module->exports));
    REQUIRE(module->exports);
    module->exports[0] = (XiModuleExport) {
        .name = "xtp_probe",
        .shared_slot = 0,
        .function = function,
    };
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrSemanticPlan *semantic = xr_semantic_plan_retain(root->semantic_plan);
    REQUIRE(semantic && xr_semantic_plan_source_export_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static XrSemanticPlan *build_direct_call_semantic_plan(void) {
    XiFunc *root = xi_func_new("xtp_direct_call_root", &stub_int);
    XiFunc *child = xi_func_new("xtp_direct_call_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params != NULL);
    child->params[0] = xi_param(child, child_entry, 0, &stub_int);
    REQUIRE(child->params[0] != NULL);
    xi_block_set_return(child_entry, child->params[0]);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    REQUIRE(closure != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    XiValue *argument = xi_const_int(root, root_entry, 9, &stub_int);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(argument != NULL && call != NULL);
    call->args[0] = alias;
    call->args[1] = argument;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static XrSemanticPlan *build_channel_close_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_channel_close", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *capacity = xi_const_int(function, entry, 1, &stub_int);
    XiValue *channel =
        xi_value_new(function, entry, XI_CHAN_NEW, &stub_channel, 1);
    XiValue *alias =
        xi_value_new(function, entry, XI_COPY, &stub_channel, 1);
    XiValue *close =
        xi_value_new(function, entry, XI_CALL_METHOD, &stub_unit, 1);
    REQUIRE(capacity != NULL && channel != NULL && alias != NULL &&
            close != NULL);
    channel->args[0] = capacity;
    alias->args[0] = channel;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    close->args[0] = alias;
    close->aux = (void *) "close";
    close->aux_int = 314;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error,
                                   sizeof(error)));
    REQUIRE(semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_source_export_semantic_plan(
    XrSemanticPlan **dependency_out) {
    XiFunc *dependency_root = xi_func_new("xtp_net_init", &stub_unit);
    XiFunc *write_bytes = xi_func_new("writeBytes", &stub_unit);
    REQUIRE(dependency_root && write_bytes);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *write_entry = xi_block_new(write_bytes);
    REQUIRE(dependency_entry && write_entry);
    dependency_entry->sealed = write_entry->sealed = true;
    dependency_root->children =
        (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = write_bytes;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    write_bytes->parent_func = dependency_root;
    XiValue *closure = xi_value_new(dependency_root, dependency_entry,
                                    XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry,
                                  XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(closure && store);
    closure->aux = write_bytes;
    store->args[0] = closure;
    store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    REQUIRE(xi_value_new(write_bytes, write_entry, XI_YIELD, &stub_unit, 0));
    xi_block_set_return(write_entry, NULL);
    dependency_root->stage = write_bytes->stage = XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = write_bytes->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = write_bytes->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module =
        xi_module_new("stdlib/net/net.xr", "net", dependency_root);
    REQUIRE(dependency_module);
    REQUIRE(xi_module_set_identity(
        dependency_module, "stdlib-module-v1:module=3:net:path=10:net/net.xr"));
    dependency_root->module = dependency_module;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports);
    dependency_module->exports[0].name = "writeBytes";
    dependency_module->exports[0].shared_slot = 0;
    dependency_module->exports[0].function = write_bytes;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, error,
                                              sizeof(error)));
    XrSemanticPlan *dependency =
        xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency);

    XiFunc *caller_root = xi_func_new("xtp_http_init", &stub_unit);
    XiFunc *caller = xi_func_new("_serverWriteAll", &stub_unit);
    REQUIRE(caller_root && caller);
    XiBlock *root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    caller_root->children =
        (XiFunc **) xr_calloc(1, sizeof(*caller_root->children));
    REQUIRE(caller_root->children);
    caller_root->children[0] = caller;
    caller_root->nchildren = caller_root->children_cap = 1;
    caller->parent_func = caller_root;
    XiImportRef import_ref = {
        .module_path = "stdlib-module-v1:module=3:net:path=10:net/net.xr",
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = dependency_module,
    };
    XiValue *namespace_ref = xi_value_new(caller_root, root_entry,
                                          XI_IMPORT_REF,
                                          &stub_module_namespace, 0);
    XiValue *namespace_store = xi_value_new(caller_root, root_entry,
                                            XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(namespace_ref && namespace_store);
    namespace_ref->aux = &import_ref;
    namespace_store->args[0] = namespace_ref;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver = xi_value_new(caller, caller_entry, XI_GET_SHARED,
                                     &stub_module_namespace, 0);
    XiValue *method = xi_value_new(caller, caller_entry, XI_CALL_METHOD,
                                   &stub_unit, 1);
    REQUIRE(receiver && method);
    receiver->aux_int = 0;
    method->args[0] = receiver;
    method->aux = (void *) "writeBytes";
    method->aux_int = 0;
    xi_block_set_return(caller_entry, method);
    caller_root->stage = caller->stage = XI_STAGE_SEMANTIC_LOWERED;
    caller_root->invariant_mask = caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    XiCoroResolver resolver = {
        .resolve_method = source_export_resolve_method,
        .call_suspendability = source_export_call_suspendability,
        .ud = write_bytes,
    };
    REQUIRE(xi_coro_lower(caller_root, &resolver));
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module =
        xi_module_new("stdlib/http/http.xr", "http", caller_root);
    REQUIRE(caller_module);
    REQUIRE(xi_module_set_identity(
        caller_module, "stdlib-module-v1:module=4:http:path=12:http/http.xr"));
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependency_modules[] = {dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(
        caller_root, dependency_modules, 1, error, sizeof(error)));
    XrSemanticPlan *semantic =
        xr_semantic_plan_retain(caller_root->semantic_plan);
    REQUIRE(semantic);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
    *dependency_out = dependency;
    return semantic;
}

static XrSemanticPlan *build_coroutine_call_semantic_plan(void) {
    XiFunc *root = xi_func_new("xtp_coroutine_root", &stub_int);
    XiFunc *child = xi_func_new("xtp_coroutine_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    root_entry->sealed = child_entry->sealed = true;
    XiValue *yield = xi_value_new(child, child_entry, XI_YIELD, &stub_unit, 0);
    XiValue *child_result = xi_const_int(child, child_entry, 31, &stub_int);
    REQUIRE(yield != NULL && child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure =
        xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 1);
    REQUIRE(closure != NULL && call != NULL);
    closure->aux = child;
    call->args[0] = closure;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = child->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static XrTargetProfile *build_profile(void) {
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization =
            &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, error, sizeof(error)));
    return profile;
}

typedef enum NativeMachineMutation {
    NATIVE_MACHINE_ARCHITECTURE,
    NATIVE_MACHINE_OPERATING_SYSTEM,
    NATIVE_MACHINE_ENVIRONMENT,
    NATIVE_MACHINE_ABI,
    NATIVE_MACHINE_DATA_LAYOUT,
    NATIVE_MACHINE_ATOMIC_WIDTHS,
    NATIVE_MACHINE_ATOMIC_ORDERS,
    NATIVE_MACHINE_FLOAT_FEATURES,
    NATIVE_MACHINE_VECTOR_FEATURES,
    NATIVE_MACHINE_VECTOR_WIDTH,
    NATIVE_MACHINE_RUNTIME_PROFILE,
    NATIVE_MACHINE_MUTATION_COUNT,
} NativeMachineMutation;

typedef enum ForeignProfileMutation {
    FOREIGN_PROFILE_ARCHITECTURE_ABI,
    FOREIGN_PROFILE_PLATFORM_IDENTITY,
    FOREIGN_PROFILE_DATA_LAYOUT,
    FOREIGN_PROFILE_ATOMIC_WIDTHS,
    FOREIGN_PROFILE_ATOMIC_ORDERS,
    FOREIGN_PROFILE_FLOAT_FEATURES,
    FOREIGN_PROFILE_VECTOR_FEATURES,
    FOREIGN_PROFILE_RUNTIME_PROFILE,
    FOREIGN_PROFILE_MUTATION_COUNT,
} ForeignProfileMutation;

static void mutate_exact_machine_field(XrTargetMachineFacts *machine,
                                       NativeMachineMutation mutation) {
    switch (mutation) {
        case NATIVE_MACHINE_ARCHITECTURE:
            machine->architecture = machine->architecture == XR_TARGET_ARCH_X86_64
                                        ? XR_TARGET_ARCH_AARCH64
                                        : XR_TARGET_ARCH_X86_64;
            break;
        case NATIVE_MACHINE_OPERATING_SYSTEM:
            machine->operating_system = machine->operating_system == XR_TARGET_OS_WINDOWS
                                            ? XR_TARGET_OS_LINUX
                                            : XR_TARGET_OS_WINDOWS;
            break;
        case NATIVE_MACHINE_ENVIRONMENT:
            machine->environment = machine->environment == XR_TARGET_ENV_MSVC
                                       ? XR_TARGET_ENV_GNU
                                       : XR_TARGET_ENV_MSVC;
            break;
        case NATIVE_MACHINE_ABI:
            machine->native_abi = machine->native_abi == XR_TARGET_ABI_WIN64_X86_64
                                      ? XR_TARGET_ABI_SYSV_X86_64
                                      : XR_TARGET_ABI_WIN64_X86_64;
            break;
        case NATIVE_MACHINE_DATA_LAYOUT:
            machine->data_layout.stable_hash ^= UINT64_C(1);
            break;
        case NATIVE_MACHINE_ATOMIC_WIDTHS:
            machine->atomic_width_mask ^= XR_TARGET_ATOMIC_WIDTH_128;
            break;
        case NATIVE_MACHINE_ATOMIC_ORDERS:
            machine->atomic_order_mask ^= XR_TARGET_ATOMIC_SEQ_CST;
            break;
        case NATIVE_MACHINE_FLOAT_FEATURES:
            machine->float_feature_mask ^= XR_TARGET_FLOAT_FMA;
            break;
        case NATIVE_MACHINE_VECTOR_FEATURES:
            machine->vector_feature_mask ^= XR_TARGET_VECTOR_SSE2;
            break;
        case NATIVE_MACHINE_VECTOR_WIDTH:
            machine->maximum_vector_bits ^= 128u;
            break;
        case NATIVE_MACHINE_RUNTIME_PROFILE:
            machine->runtime_profile =
                machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED
                    ? XR_TARGET_RUNTIME_PROFILE_FREESTANDING
                    : XR_TARGET_RUNTIME_PROFILE_HOSTED;
            break;
        case NATIVE_MACHINE_MUTATION_COUNT:
            abort();
    }
}

static void set_native_abi_for_platform(XrTargetMachineFacts *machine) {
    if (machine->operating_system == XR_TARGET_OS_WINDOWS) {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_WIN64_AARCH64
                                  : XR_TARGET_ABI_WIN64_X86_64;
    } else if (machine->operating_system == XR_TARGET_OS_MACOS) {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_DARWIN_AARCH64
                                  : XR_TARGET_ABI_DARWIN_X86_64;
    } else {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_AAPCS64
                                  : XR_TARGET_ABI_SYSV_X86_64;
    }
}

static void set_supported_vector_profile(XrTargetMachineFacts *machine) {
    switch (machine->architecture) {
        case XR_TARGET_ARCH_X86_64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_SSE2;
            break;
        case XR_TARGET_ARCH_AARCH64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_NEON;
            break;
        case XR_TARGET_ARCH_POWERPC64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_VSX;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_LSX;
            break;
        default:
            abort();
    }
    machine->maximum_vector_bits = 128;
}

static void mutate_foreign_profile(XrTargetMachineFacts *machine,
                                   ForeignProfileMutation mutation) {
    switch (mutation) {
        case FOREIGN_PROFILE_ARCHITECTURE_ABI:
            machine->architecture = machine->architecture == XR_TARGET_ARCH_X86_64
                                        ? XR_TARGET_ARCH_AARCH64
                                        : XR_TARGET_ARCH_X86_64;
            set_native_abi_for_platform(machine);
            break;
        case FOREIGN_PROFILE_PLATFORM_IDENTITY:
            if (machine->operating_system == XR_TARGET_OS_WINDOWS) {
                machine->operating_system = XR_TARGET_OS_LINUX;
                machine->environment = XR_TARGET_ENV_GNU;
            } else {
                machine->operating_system = XR_TARGET_OS_WINDOWS;
                machine->environment = XR_TARGET_ENV_MSVC;
            }
            if (machine->architecture != XR_TARGET_ARCH_X86_64 &&
                machine->architecture != XR_TARGET_ARCH_AARCH64)
                machine->architecture = XR_TARGET_ARCH_X86_64;
            set_native_abi_for_platform(machine);
            break;
        case FOREIGN_PROFILE_DATA_LAYOUT:
            machine->data_layout.i16.align =
                machine->data_layout.i16.align == 1 ? 2 : 1;
            machine->data_layout.stable_hash =
                xr_target_data_layout_hash(&machine->data_layout);
            break;
        case FOREIGN_PROFILE_ATOMIC_WIDTHS:
            machine->atomic_width_mask ^= XR_TARGET_ATOMIC_WIDTH_128;
            break;
        case FOREIGN_PROFILE_ATOMIC_ORDERS:
            machine->atomic_order_mask ^= XR_TARGET_ATOMIC_SEQ_CST;
            break;
        case FOREIGN_PROFILE_FLOAT_FEATURES:
            machine->float_feature_mask ^= XR_TARGET_FLOAT_FMA;
            break;
        case FOREIGN_PROFILE_VECTOR_FEATURES:
            set_supported_vector_profile(machine);
            break;
        case FOREIGN_PROFILE_RUNTIME_PROFILE:
            machine->runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
            break;
        case FOREIGN_PROFILE_MUTATION_COUNT:
            abort();
    }
}

static XrTargetProfile *build_foreign_profile(
    const XrTargetProfile *native_profile, ForeignProfileMutation mutation) {
    XrTargetProfileDraft draft = *xr_target_profile_facts(native_profile);
    mutate_foreign_profile(&draft.machine, mutation);
    XrTargetProfile *foreign = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_freeze(&draft, &foreign, error, sizeof(error)));
    REQUIRE(foreign != NULL);
    REQUIRE(xr_target_profile_verify(foreign, error, sizeof(error)));
    return foreign;
}

static XtpFixture make_fixture_from_semantic(XrSemanticPlan *semantic) {
    XtpFixture fixture = {0};
    fixture.semantic = semantic;
    fixture.profile = build_profile();
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                                 sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error,
                               sizeof(error)));
    REQUIRE(fixture.bytes != NULL && fixture.size >= XR_XTP_HEADER_SIZE);
    return fixture;
}


static XtpFixture make_fixture(void) {
    return make_fixture_from_semantic(build_semantic_plan());
}

static XtpFixture make_direct_call_fixture(void) {
    return make_fixture_from_semantic(build_direct_call_semantic_plan());
}

static XtpFixture make_coroutine_call_fixture(void) {
    return make_fixture_from_semantic(build_coroutine_call_semantic_plan());
}

static XtpFixture make_channel_close_fixture(void) {
    return make_fixture_from_semantic(build_channel_close_semantic_plan());
}

static XtpFixture make_stringbuilder_fixture(void) {
    return make_fixture_from_semantic(build_stringbuilder_semantic_plan());
}

static XtpFixture make_string_concat_release_fixture(void) {
    return make_fixture_from_semantic(
        build_string_concat_release_semantic_plan());
}

static XtpFixture make_string_coroutine_lifecycle_fixture(void) {
    return make_fixture_from_semantic(
        build_string_coroutine_lifecycle_semantic_plan());
}

static XtpFixture make_source_export_fixture(void) {
    XtpFixture fixture = {0};
    fixture.semantic =
        build_source_export_semantic_plan(&fixture.dependency);
    fixture.profile = build_profile();
    const XrSemanticPlan *dependencies[] = {fixture.dependency};
    char error[512] = {0};
    REQUIRE(xr_target_plan_build_module_set(
        fixture.semantic, dependencies, 1, fixture.profile, &fixture.plan,
        error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size,
                               error, sizeof(error)));
    REQUIRE(fixture.bytes && fixture.size >= XR_XTP_HEADER_SIZE);
    return fixture;
}

static void dispose_fixture(XtpFixture *fixture) {
    xr_xtp_encoded_free(fixture->bytes);
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    xr_semantic_plan_free(fixture->dependency);
    memset(fixture, 0, sizeof(*fixture));
}

static uint8_t *copy_artifact(const XtpFixture *fixture) {
    uint8_t *copy = (uint8_t *) xr_malloc(fixture->size);
    REQUIRE(copy != NULL);
    memcpy(copy, fixture->bytes, fixture->size);
    return copy;
}

static void resign_artifact(uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static uint8_t *directory_entry(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE +
           ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
}

static void resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static void expect_decode_failure(const uint8_t *bytes, size_t size) {
    XrXtpCandidate *candidate = (XrXtpCandidate *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error)));
    REQUIRE(candidate == NULL);
    REQUIRE(strncmp(error, "XR_", 3) == 0);
}

static void expect_materialize_failure(const XtpFixture *fixture, const uint8_t *bytes) {
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(bytes, fixture->size, &candidate, error, sizeof(error)));
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_xtp_materialize_target_plan(candidate, fixture->semantic, fixture->profile,
                                             &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_", 3) == 0);
    xr_xtp_candidate_release(candidate);
}

static void expect_decode_or_materialize_failure(const XtpFixture *fixture,
                                                 const uint8_t *bytes) {
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    if (!xr_xtp_decode_candidate(bytes, fixture->size, &candidate, error,
                                 sizeof(error))) {
        REQUIRE(candidate == NULL && strncmp(error, "XR_", 3) == 0);
        return;
    }
    REQUIRE(!xr_xtp_materialize_target_plan(candidate, fixture->semantic,
                                             fixture->profile, &plan, error,
                                             sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_", 3) == 0);
    xr_xtp_candidate_release(candidate);
}

static void test_exact_roundtrip_and_owned_candidate(void) {
    XtpFixture fixture = make_fixture();
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate, error,
                                    sizeof(error)));
    REQUIRE(xr_xtp_candidate_retain(candidate) == candidate);
    xr_xtp_candidate_release(candidate);
    XrXtpIdentity identity;
    XrXtpResourceManifest resources;
    REQUIRE(xr_xtp_candidate_identity(candidate, &identity));
    REQUIRE(xr_xtp_candidate_resources(candidate, &resources));
    REQUIRE(identity.plan_schema == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(identity.completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(resources.total_rows > 1 &&
            resources.verification_work_units > resources.total_rows);

    uint8_t saved = fixture.bytes[0];
    uint8_t saved_schema = fixture.bytes[4];
    uint8_t saved_digest = fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET];
    uint8_t *function_entry =
        directory_entry(fixture.bytes, XR_XTP_SECTION_FUNCTIONS);
    size_t function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    uint8_t saved_function = fixture.bytes[function_offset];
    fixture.bytes[0] ^= 0xff;
    fixture.bytes[4] ^= 0xff;
    fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET] ^= 0xff;
    fixture.bytes[function_offset] ^= 0xff;
    XrTargetPlan *decoded_plan = NULL;
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic, fixture.profile,
                                            &decoded_plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(decoded_plan));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded_plan),
                                 xr_target_plan_fingerprint(fixture.plan)));
    fixture.bytes[0] = saved;
    fixture.bytes[4] = saved_schema;
    fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET] = saved_digest;
    fixture.bytes[function_offset] = saved_function;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    REQUIRE(xr_xtp_encode_plan(decoded_plan, &encoded, &encoded_size, error, sizeof(error)));
    REQUIRE(encoded_size == fixture.size);
    REQUIRE(memcmp(encoded, fixture.bytes, fixture.size) == 0);
    xr_xtp_encoded_free(encoded);
    xr_target_plan_free(decoded_plan);
    xr_xtp_candidate_release(candidate);

    const uint32_t rejected_schemas[] = {UINT32_C(12), UINT32_C(13), UINT32_C(14),
                                         UINT32_C(28), UINT32_C(41), UINT32_C(42),
                                         UINT32_C(43), UINT32_C(44), UINT32_C(45)};
    for (size_t i = 0;
         i < sizeof(rejected_schemas) / sizeof(rejected_schemas[0]); i++) {
        uint8_t *old_schema = copy_artifact(&fixture);
        xr_xtp_put_u32(old_schema + 4, rejected_schemas[i]);
        resign_artifact(old_schema, fixture.size);
        expect_decode_failure(old_schema, fixture.size);
        xr_free(old_schema);
    }

    uint8_t *mutated_profile = copy_artifact(&fixture);
    uint8_t *profile_entry =
        directory_entry(mutated_profile, XR_XTP_SECTION_TARGET_PROFILE);
    size_t profile_offset =
        (size_t) xr_xtp_take_u64(profile_entry + 8);
    /* Profile v2: dynamic_tag immediately follows the 292-byte v1 prefix
     * and the new literal contract schema. */
    mutated_profile[profile_offset + 296] ^= 1;
    resign_section(mutated_profile, XR_XTP_SECTION_TARGET_PROFILE);
    resign_artifact(mutated_profile, fixture.size);
    expect_materialize_failure(&fixture, mutated_profile);
    xr_free(mutated_profile);
    dispose_fixture(&fixture);
}

typedef struct ConcurrentCandidateContext {
    XrXtpCandidate *candidate;
    const XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrFingerprint expected_fingerprint;
    bool completed;
} ConcurrentCandidateContext;

static void *materialize_candidate_thread(void *opaque) {
    enum { ITERATIONS = 24 };
    ConcurrentCandidateContext *context =
        (ConcurrentCandidateContext *) opaque;
    for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
        XrXtpCandidate *retained =
            xr_xtp_candidate_retain(context->candidate);
        XrTargetPlan *plan = NULL;
        XrXtpIdentity identity = {0};
        XrXtpResourceManifest resources = {0};
        char error[256] = {0};
        bool valid =
            retained && xr_xtp_candidate_identity(retained, &identity) &&
            xr_xtp_candidate_resources(retained, &resources) &&
            identity.plan_schema == XR_TARGET_PLAN_SCHEMA_VERSION &&
            resources.total_rows != 0 &&
            xr_xtp_materialize_target_plan(
                retained, context->semantic, context->profile, &plan, error,
                sizeof(error)) &&
            plan && xr_target_plan_is_verified(plan) &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                                 context->expected_fingerprint);
        xr_target_plan_free(plan);
        xr_xtp_candidate_release(retained);
        if (!valid)
            return NULL;
    }
    context->completed = true;
    return NULL;
}

static void test_concurrent_candidate_materialization(void) {
    enum { THREAD_COUNT = 8 };
    XtpFixture fixture = make_fixture();
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    ConcurrentCandidateContext contexts[THREAD_COUNT] = {0};
    xr_thread_t threads[THREAD_COUNT] = {0};
    XrFingerprint expected = xr_target_plan_fingerprint(fixture.plan);
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        contexts[i] = (ConcurrentCandidateContext) {
            .candidate = candidate,
            .semantic = fixture.semantic,
            .profile = fixture.profile,
            .expected_fingerprint = expected,
        };
        REQUIRE(xr_thread_create(&threads[i], materialize_candidate_thread,
                                 &contexts[i]));
    }
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(contexts[i].completed);
    }
    xr_xtp_candidate_release(candidate);
    dispose_fixture(&fixture);
}

static void test_direct_call_rows_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_direct_call_fixture();
    uint32_t count = 0;
    REQUIRE(xr_target_plan_calls(fixture.plan, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_call_arguments(fixture.plan, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_adapters(fixture.plan, &count) == NULL && count == 0);
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                            fixture.profile, &decoded,
                                            error, sizeof(error)));
    REQUIRE(xr_target_plan_calls(decoded, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_call_arguments(decoded, &count) != NULL && count == 1);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *call_entry = directory_entry(copy, XR_XTP_SECTION_CALLS);
    size_t call_offset = (size_t) xr_xtp_take_u64(call_entry + 8);
    copy[call_offset + 20] ^= 1; /* semantic_call_target */
    resign_section(copy, XR_XTP_SECTION_CALLS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    uint8_t *argument_entry = directory_entry(copy, XR_XTP_SECTION_CALL_ARGUMENTS);
    size_t argument_offset = (size_t) xr_xtp_take_u64(argument_entry + 8);
    copy[argument_offset + 20] ^= 1; /* semantic_operand */
    resign_section(copy, XR_XTP_SECTION_CALL_ARGUMENTS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static void test_channel_close_row_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_channel_close_fixture();
    uint32_t count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(fixture.plan, &count);
    REQUIRE(calls != NULL && count == 1 &&
            calls[0].semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            calls[0].callee_function == XR_SEMANTIC_INDEX_NONE &&
            calls[0].caller_storage_slot == XR_SEMANTIC_INDEX_NONE &&
            calls[0].argument_count == 0 &&
            calls[0].calling_convention ==
                XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE &&
            calls[0].target_kind == XR_TARGET_CALL_TARGET_CHANNEL_CLOSE);
    REQUIRE(xr_target_plan_call_arguments(fixture.plan, &count) == NULL &&
            count == 0);

    uint8_t *call_entry =
        directory_entry(fixture.bytes, XR_XTP_SECTION_CALLS);
    size_t call_offset = (size_t) xr_xtp_take_u64(call_entry + 8);
    REQUIRE(xr_xtp_take_u32(fixture.bytes + call_offset + 20) ==
                XR_SEMANTIC_INDEX_NONE &&
            xr_xtp_take_u32(fixture.bytes + call_offset + 32) ==
                XR_SEMANTIC_INDEX_NONE &&
            xr_xtp_take_u32(fixture.bytes + call_offset + 84) ==
                XR_SEMANTIC_INDEX_NONE &&
            xr_xtp_take_u16(fixture.bytes + call_offset + 108) == 0 &&
            fixture.bytes[call_offset + 116] ==
                XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE &&
            fixture.bytes[call_offset + 117] ==
                XR_TARGET_CALL_TARGET_CHANNEL_CLOSE);

    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                           fixture.profile, &decoded, error,
                                           sizeof(error)));
    const XrTargetCallRecord *decoded_calls =
        xr_target_plan_calls(decoded, &count);
    REQUIRE(decoded != NULL && decoded_calls != NULL && count == 1 &&
            decoded_calls[0].target_kind ==
                XR_TARGET_CALL_TARGET_CHANNEL_CLOSE &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        20, /* semantic_call_target */
        32, /* callee_function */
        84, /* caller_storage_slot */
        108, /* argument_count */
        114, /* flags */
        116, /* calling_convention */
        117, /* target_kind */
    };
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_CALLS);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CALLS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_stringbuilder_row_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_stringbuilder_fixture();
    uint32_t count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(fixture.plan, &count);
    REQUIRE(calls && count == 1 &&
            calls[0].semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            calls[0].argument_count == 0 && calls[0].flags == 0 &&
            calls[0].result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            calls[0].calling_convention ==
                XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR &&
            calls[0].target_kind ==
                XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR);
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                            fixture.profile, &decoded, error,
                                            sizeof(error)));
    const XrTargetCallRecord *decoded_calls =
        xr_target_plan_calls(decoded, &count);
    REQUIRE(decoded_calls && count == 1 &&
            decoded_calls[0].target_kind ==
                XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR &&
            decoded_calls[0].result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        0,   /* identity */
        20,  /* semantic_call_target */
        76,  /* result_slot */
        96,  /* result_register_rep */
        116, /* calling_convention */
        117, /* target_kind */
        119, /* result_ownership */
    };
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_CALLS);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CALLS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_cleanup_row_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_string_concat_release_fixture();
    uint32_t count = 0;
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(fixture.plan, &count);
    REQUIRE(cleanups != NULL && count == 1 && cleanups[0].id == 0 &&
            cleanups[0].function == 0 &&
            cleanups[0].action == XR_TARGET_CLEANUP_RELEASE &&
            cleanups[0].flags == 0 && cleanups[0].provider == 0);

    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                            fixture.profile, &decoded, error,
                                            sizeof(error)));
    const XrTargetCleanupRecord *decoded_cleanups =
        xr_target_plan_cleanups(decoded, &count);
    REQUIRE(decoded_cleanups != NULL && count == 1 &&
            decoded_cleanups[0].semantic_operation ==
                cleanups[0].semantic_operation &&
            decoded_cleanups[0].slot == cleanups[0].slot &&
            decoded_cleanups[0].action == XR_TARGET_CLEANUP_RELEASE &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        8,  /* semantic_operation */
        12, /* slot */
        16, /* action */
        18, /* provider */
    };
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_CLEANUPS);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CLEANUPS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_coroutine_lifecycle_rows_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_string_coroutine_lifecycle_fixture();
    uint32_t count = 0;
    const XrTargetRootMapRecord *roots =
        xr_target_plan_root_maps(fixture.plan, &count);
    REQUIRE(roots && count == 1 && roots[0].slot_count == 1 &&
            roots[0].flags == (XR_TARGET_ROOT_SUSPEND |
                               XR_TARGET_ROOT_CANCEL |
                               XR_TARGET_ROOT_EXIT));
    const uint32_t *root_slots =
        xr_target_plan_root_slots(fixture.plan, &count);
    REQUIRE(root_slots && count == 1);
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(fixture.plan, &count);
    REQUIRE(cleanups && count == 2 &&
            cleanups[0].semantic_operation == roots[0].semantic_operation &&
            cleanups[0].slot == root_slots[0] &&
            cleanups[0].flags ==
                (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT) &&
            cleanups[1].flags == 0);

    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                            fixture.profile, &decoded, error,
                                            sizeof(error)));
    const XrTargetRootMapRecord *decoded_roots =
        xr_target_plan_root_maps(decoded, &count);
    REQUIRE(decoded_roots && count == 1 &&
            decoded_roots[0].semantic_operation == roots[0].semantic_operation &&
            decoded_roots[0].flags == roots[0].flags &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    const struct {
        XrXtpSectionKind section;
        size_t byte;
    } mutations[] = {
        {XR_XTP_SECTION_ROOT_MAPS, 18},
        {XR_XTP_SECTION_ROOT_SLOTS, 0},
        {XR_XTP_SECTION_CLEANUPS, 17},
    };
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, mutations[i].section);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i].byte] ^= 1;
        resign_section(copy, mutations[i].section);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_source_export_row_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_source_export_fixture();
    uint32_t count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(fixture.plan, &count);
    REQUIRE(calls && count == 1 && calls[0].source_dependency == 0 &&
            calls[0].source_export == 0 &&
            calls[0].callee_function == XR_SEMANTIC_INDEX_NONE &&
            calls[0].argument_count == 0 &&
            calls[0].calling_convention ==
                XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
            calls[0].target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
            calls[0].flags == XR_TARGET_CALL_SUSPEND);
    const XrSemanticPlan *dependencies[] = {fixture.dependency};
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(!xr_xtp_materialize_target_plan(
        candidate, fixture.semantic, fixture.profile, &decoded, error,
        sizeof(error)));
    REQUIRE(decoded == NULL);
    REQUIRE(xr_xtp_materialize_target_plan_module_set(
        candidate, fixture.semantic, dependencies, 1, fixture.profile,
        &decoded, error, sizeof(error)));
    const XrTargetCallRecord *decoded_calls =
        xr_target_plan_calls(decoded, &count);
    REQUIRE(decoded_calls && count == 1 &&
            decoded_calls[0].source_dependency == 0 &&
            xr_stable_id_equal(decoded_calls[0].source_export_identity,
                               calls[0].source_export_identity) &&
            xr_stable_id_equal(decoded_calls[0].source_callee_identity,
                               calls[0].source_callee_identity) &&
            xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        36, /* source_dependency */
        40, /* source_export */
        44, /* source_export_identity */
        60, /* source_callee_identity */
        76, /* result_value */
        80, /* result_slot */
        108, /* argument_count */
        114, /* flags */
        116, /* calling_convention */
        117, /* target_kind */
    };
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_CALLS);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CALLS);
        resign_artifact(copy, fixture.size);
        candidate = NULL;
        REQUIRE(xr_xtp_decode_candidate(copy, fixture.size, &candidate, error,
                                        sizeof(error)));
        decoded = (XrTargetPlan *) (uintptr_t) 1;
        REQUIRE(!xr_xtp_materialize_target_plan_module_set(
            candidate, fixture.semantic, dependencies, 1, fixture.profile,
            &decoded, error, sizeof(error)));
        REQUIRE(decoded == NULL);
        xr_xtp_candidate_release(candidate);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_coroutine_rows_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_coroutine_call_fixture();
    uint32_t count = 0;
    REQUIRE(xr_target_plan_coroutines(fixture.plan, &count) != NULL &&
            count == 2);
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                           fixture.profile, &decoded,
                                           error, sizeof(error)));
    REQUIRE(xr_target_plan_coroutines(decoded, &count) != NULL && count == 2);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        8,  /* semantic_entity */
        12, /* semantic_operation */
        16, /* logical_state */
        20, /* suspend_block */
        24, /* resume_block */
        28, /* resume_predecessor */
        32, /* resume_instruction */
        36, /* direct_call */
        40, /* result_slot */
        44, /* resume_predecessor_ordinal */
        46, /* flags */
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_COROUTINES);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_COROUTINES);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_artifact_classifier(void) {
    static const uint8_t source[] = "print(1)";
    static const uint8_t removed_xtp_v1[] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 0};
    static const uint8_t removed_xray_container[] = {
        'X', 'R', 'A', 'Y', 30, 0};
    static const uint8_t corrupt_removed[] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 1};
    static const uint8_t unknown_reserved[] = {
        'X', 'R', 'A', 'Y', 'Q', 'Q', 'Q', 0};
    XrArtifactProbeResult probe =
        xr_artifact_probe("renamed.bin", xr_xsm_artifact_magic,
                          XR_XSM_ARTIFACT_MAGIC_SIZE);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_XSM);
    probe = xr_artifact_probe("renamed.bin", xr_xtp_artifact_magic,
                              XR_XTP_ARTIFACT_MAGIC_SIZE);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_XTP);
    probe = xr_artifact_probe("renamed.bin", removed_xray_container,
                              sizeof(removed_xray_container));
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("removed.container", source,
                              sizeof(source) - 1).status ==
            XR_ARTIFACT_PROBE_MATCH);
    probe = xr_artifact_probe("program.xr", source, sizeof(source) - 1);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_SOURCE);
    REQUIRE(xr_artifact_probe("program.xtp", source, sizeof(source) - 1).status ==
            XR_ARTIFACT_PROBE_CONFLICT);
    REQUIRE(xr_artifact_probe("program.xsm", xr_xtp_artifact_magic,
                              XR_XTP_ARTIFACT_MAGIC_SIZE).status ==
            XR_ARTIFACT_PROBE_CONFLICT);
    REQUIRE(xr_artifact_probe("renamed.bin", removed_xtp_v1,
                              sizeof(removed_xtp_v1)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("renamed.bin", corrupt_removed,
                              sizeof(corrupt_removed)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("renamed.bin", unknown_reserved,
                              sizeof(unknown_reserved)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    for (size_t size = 5; size < XR_XSM_ARTIFACT_MAGIC_SIZE; size++)
        REQUIRE(xr_artifact_probe("renamed.bin", xr_xsm_artifact_magic,
                                  size).status ==
                XR_ARTIFACT_PROBE_NEED_MORE);
}

static void test_runtime_machine_authority_is_exact_and_scalar_only(void) {
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(authority.machine.runtime_profile ==
            XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(authority.machine.vector_feature_mask == 0);
    REQUIRE(authority.machine.maximum_vector_bits == 0);
    REQUIRE(xr_runtime_target_authority_machine_matches(
        &authority, &authority.machine));
    for (NativeMachineMutation mutation = 0;
         mutation < NATIVE_MACHINE_MUTATION_COUNT; mutation++) {
        XrTargetMachineFacts candidate = authority.machine;
        mutate_exact_machine_field(&candidate, mutation);
        REQUIRE(!xr_runtime_target_authority_machine_matches(&authority,
                                                              &candidate));
    }
}

static void test_runtime_factory_owns_native_profile(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    REQUIRE(authority->target_profile != fixture.profile);
    REQUIRE(xr_target_profile_require_exact(
        fixture.profile, authority->target_profile, diagnostic,
        sizeof(diagnostic)));
    XrRuntimeTargetAuthority runtime;
    REQUIRE(xr_runtime_target_authority_native_hosted(&runtime) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_target_authority_machine_matches(
        &runtime, xr_target_profile_machine_facts(authority->target_profile)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_assertion_artifact_capability_identity(void) {
    XrSemanticPlan *semantic = build_assertion_artifact_semantic_plan();
    XrRuntimeArtifactAuthority *authority = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        semantic, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    const uint64_t expected = XR_TARGET_FOUNDATION_CAPABILITY_MASK;
    REQUIRE(authority->identity.required_capability_mask == expected);
    XrRuntimeArtifactAuthorityIdentity saved = authority->identity;
    authority->identity.required_capability_mask =
        XR_TARGET_FOUNDATION_CAPABILITY_MASK |
        XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ASSERTION_REPORT);
    xr_runtime_artifact_authority_compute_fingerprint(
        &authority->identity, authority->identity.authority_fingerprint);
    REQUIRE(!xr_runtime_artifact_authority_verify(
        authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_TARGET_1004") != NULL);
    authority->identity = saved;
    REQUIRE(xr_runtime_artifact_authority_verify(
        authority, diagnostic, sizeof(diagnostic)));
    xr_runtime_artifact_authority_free(authority);
    xr_semantic_plan_free(semantic);
}

static void test_public_xsm_authority_loads_exact_semantics(void) {
    XtpFixture fixture = make_fixture();
    uint8_t *xsm = NULL;
    size_t xsm_size = 0;
    char diagnostic[512] = {0};
    REQUIRE(xr_xsm_encode(fixture.semantic, &xsm, &xsm_size, diagnostic,
                          sizeof(diagnostic)));
    REQUIRE(xr_runtime_artifact_authority_load_available());

    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_load_xsm(
        xsm, xsm_size, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    XrRuntimeArtifactAuthorityIdentity identity;
    REQUIRE(xr_runtime_artifact_authority_identity(authority, &identity));
    REQUIRE(memcmp(identity.semantic_fingerprint,
                   xr_semantic_plan_fingerprint(fixture.semantic).bytes,
                   XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE) == 0);

    XrTargetPlan *loaded = NULL;
    REQUIRE(xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded != NULL && xr_target_plan_is_verified(loaded));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(loaded),
                                 xr_target_plan_fingerprint(fixture.plan)));

    XtpFixture foreign = make_direct_call_fixture();
    XrTargetPlan *foreign_loaded = (XrTargetPlan *) (uintptr_t) 1;
    memset(diagnostic, 0, sizeof(diagnostic));
    REQUIRE(!xr_runtime_target_plan_load(
        foreign.bytes, foreign.size, authority, &foreign_loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(foreign_loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_TARGET_1000") != NULL);
    dispose_fixture(&foreign);

    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 4,
        .max_pins_per_generation = 4,
        .max_pins_by_kind = {4, 4, 4, 4, 4},
    };
    XrRuntimeGenerationAuthority *generation_authority = NULL;
    XrLoadedModuleGeneration *generation = NULL;
    int64_t result = 0;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &generation_authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_load_verified_target_plan(
        generation_authority, loaded, &generation, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == 42);
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &generation_authority, diagnostic, sizeof(diagnostic)));
    xr_target_plan_free(loaded);
    xr_runtime_artifact_authority_free(authority);

    uint8_t *old_schema = (uint8_t *) xr_malloc(xsm_size);
    REQUIRE(old_schema != NULL);
    memcpy(old_schema, xsm, xsm_size);
    old_schema[8] ^= 1;
    authority = (XrRuntimeArtifactAuthority *) (uintptr_t) 1;
    memset(diagnostic, 0, sizeof(diagnostic));
    REQUIRE(!xr_runtime_artifact_authority_load_xsm(
        old_schema, xsm_size, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2000") != NULL);
    xr_free(old_schema);

    uint8_t *corrupt = (uint8_t *) xr_malloc(xsm_size);
    REQUIRE(corrupt != NULL);
    memcpy(corrupt, xsm, xsm_size);
    corrupt[XR_XSM_HEADER_SIZE] ^= 1;
    authority = (XrRuntimeArtifactAuthority *) (uintptr_t) 1;
    memset(diagnostic, 0, sizeof(diagnostic));
    REQUIRE(!xr_runtime_artifact_authority_load_xsm(
        corrupt, xsm_size, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2002") != NULL);
    xr_free(corrupt);

    authority = (XrRuntimeArtifactAuthority *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_artifact_authority_load_xsm(
        fixture.bytes, fixture.size, &authority, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(authority == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2000") != NULL);
    xr_free(xsm);
    dispose_fixture(&fixture);
}

static void test_independent_verifier_rejects_forged_machine_profiles(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    XrTargetProfile *native_profile = authority->target_profile;
    XrRuntimeArtifactAuthorityIdentity native_identity = authority->identity;

    for (ForeignProfileMutation mutation = 0;
         mutation < FOREIGN_PROFILE_MUTATION_COUNT; mutation++) {
        XrTargetProfile *foreign =
            build_foreign_profile(fixture.profile, mutation);
        authority->target_profile = foreign;
        XrFingerprint foreign_fingerprint =
            xr_target_profile_fingerprint(foreign);
        memcpy(authority->identity.target_profile_fingerprint,
               foreign_fingerprint.bytes,
               XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
        xr_runtime_artifact_authority_compute_fingerprint(
            &authority->identity, authority->identity.authority_fingerprint);
        memset(diagnostic, 0, sizeof(diagnostic));
        REQUIRE(!xr_runtime_artifact_authority_verify(
            authority, diagnostic, sizeof(diagnostic)));
        REQUIRE(strstr(diagnostic, "canonical native machine") != NULL);
        authority->target_profile = native_profile;
        authority->identity = native_identity;
        xr_target_profile_free(foreign);
    }

    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_runtime_load_rejects_foreign_profile_artifacts(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    for (ForeignProfileMutation mutation = 0;
         mutation < FOREIGN_PROFILE_MUTATION_COUNT; mutation++) {
        XrTargetProfile *foreign =
            build_foreign_profile(fixture.profile, mutation);
        XrTargetPlan *foreign_plan = NULL;
        uint8_t *foreign_bytes = NULL;
        size_t foreign_size = 0;
        REQUIRE(xr_target_plan_build(fixture.semantic, foreign, &foreign_plan,
                                     diagnostic, sizeof(diagnostic)));
        REQUIRE(xr_xtp_encode_plan(foreign_plan, &foreign_bytes,
                                   &foreign_size, diagnostic,
                                   sizeof(diagnostic)));
        XrTargetPlan *loaded = (XrTargetPlan *) (uintptr_t) 1;
        memset(diagnostic, 0, sizeof(diagnostic));
        REQUIRE(!xr_runtime_target_plan_load(
            foreign_bytes, foreign_size, authority, &loaded, diagnostic,
            sizeof(diagnostic)));
        REQUIRE(loaded == NULL);
        REQUIRE(strstr(diagnostic, "XR_TARGET_1000") != NULL);
        xr_xtp_encoded_free(foreign_bytes);
        xr_target_plan_free(foreign_plan);
        xr_target_profile_free(foreign);
    }
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_runtime_load_materializes_only_verified_plan(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    XrRuntimeArtifactAuthorityIdentity identity;
    REQUIRE(xr_runtime_artifact_authority_identity(authority, &identity));
    REQUIRE(identity.schema_version ==
            XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION);
    REQUIRE(identity.required_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(identity.required_capability_mask ==
            XR_TARGET_FOUNDATION_CAPABILITY_MASK);
    REQUIRE((identity.provider_mask & identity.required_capability_mask) ==
            identity.required_capability_mask);

    XrTargetPlan *loaded = NULL;
    REQUIRE(xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded != NULL && xr_target_plan_is_verified(loaded));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(loaded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(loaded);

    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, NULL, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL);

    static const uint8_t xsm[] = {'X', 'R', 'A', 'Y', 'X', 'S', 'M', 0};
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        xsm, sizeof(xsm), authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2000") != NULL);

    uint8_t *corrupt = copy_artifact(&fixture);
    corrupt[XR_XTP_FULL_DIGEST_OFFSET] ^= 1;
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        corrupt, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strncmp(diagnostic, "XR_ARTIFACT_2002", 16) == 0);
    xr_free(corrupt);

    XrRuntimeArtifactAuthorityIdentity saved = authority->identity;
    authority->identity.semantic_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.target_profile_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.required_family_mask = 0;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.required_capability_mask &=
        ~XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.provider_set_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.authority_fingerprint[0] ^= 1;
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    authority->identity = saved;
    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_wire_row_inventory(void) {
    static const uint32_t expected[] = {
        0, 448, 58, 12, 24, 108, 28, 40, 24, 12,
        48, 58, 32, 160, 58, 20, 4, 20, 44, 12, 48, 144, 132,
    };
    REQUIRE(sizeof(expected) / sizeof(expected[0]) == XR_XTP_SECTION_COUNT);
    for (uint32_t kind = 1; kind < XR_XTP_SECTION_COUNT; kind++) {
        REQUIRE(xr_xtp_wire_row_size((XrXtpSectionKind) kind) == expected[kind]);
        REQUIRE(xr_xtp_table_count_limit((XrXtpSectionKind) kind) > 0);
    }
    REQUIRE(xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE, XR_XTP_MAX_DECODED_TABLE_BYTES));
    REQUIRE(!xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE,
        XR_XTP_MAX_DECODED_TABLE_BYTES + 1u));
    REQUIRE(!xr_xtp_runtime_peak_within_budget(SIZE_MAX, 0));
}

static void require_row_codec_roundtrip(XrXtpSectionKind kind, size_t native_size) {
    uint32_t wire_size = xr_xtp_wire_row_size(kind);
    REQUIRE(wire_size > 0 && wire_size <= 512);
    void *source = xr_malloc(native_size);
    void *decoded = xr_calloc(1, native_size);
    REQUIRE(source != NULL && decoded != NULL);
    memset(source, 0xa5, native_size);
    uint8_t first[512] = {0};
    uint8_t second[512] = {0};
    REQUIRE(xr_xtp_encode_rows(kind, source, 1, first));
    REQUIRE(xr_xtp_decode_rows(kind, first, 1, decoded));
    REQUIRE(xr_xtp_encode_rows(kind, decoded, 1, second));
    REQUIRE(memcmp(first, second, wire_size) == 0);
    xr_free(decoded);
    xr_free(source);
}

static void test_every_typed_row_codec(void) {
#define XR_XTP_ROW_ROUNDTRIP(kind, type)                                                           \
    require_row_codec_roundtrip(XR_XTP_SECTION_##kind, sizeof(type))
    XR_XTP_ROW_ROUNDTRIP(TARGET_PROFILE, XrTargetProfileDraft);
    XR_XTP_ROW_ROUNDTRIP(MACHINE_REPS, XrTargetMachineRepRecord);
    XR_XTP_ROW_ROUNDTRIP(VALUE_REPS, XrTargetValueRepRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENTS, XrTargetExtentRecord);
    XR_XTP_ROW_ROUNDTRIP(LAYOUTS, XrTargetLayoutRecord);
    XR_XTP_ROW_ROUNDTRIP(FIELDS, XrTargetFieldRecord);
    XR_XTP_ROW_ROUNDTRIP(STORAGE, XrTargetStorageRecord);
    XR_XTP_ROW_ROUNDTRIP(ALLOCATIONS, XrTargetAllocationRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENT_OPERANDS, XrTargetExtentOperandRecord);
    XR_XTP_ROW_ROUNDTRIP(FUNCTIONS, XrTargetFunctionRecord);
    XR_XTP_ROW_ROUNDTRIP(SLOTS, XrTargetSlotRecord);
    XR_XTP_ROW_ROUNDTRIP(INSTRUCTIONS, XrTargetInstructionRecord);
    XR_XTP_ROW_ROUNDTRIP(CALLS, XrTargetCallRecord);
    XR_XTP_ROW_ROUNDTRIP(CALL_ARGUMENTS, XrTargetCallArgumentRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_MAPS, XrTargetRootMapRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_SLOTS, uint32_t);
    XR_XTP_ROW_ROUNDTRIP(CLEANUPS, XrTargetCleanupRecord);
    XR_XTP_ROW_ROUNDTRIP(ADAPTERS, XrTargetAdapterRecord);
    XR_XTP_ROW_ROUNDTRIP(CAPABILITIES, XrTargetCapabilityRecord);
    XR_XTP_ROW_ROUNDTRIP(COROUTINES, XrTargetCoroutineStateRecord);
#undef XR_XTP_ROW_ROUNDTRIP
}

static void require_instruction_stream_reencodes(
    const uint8_t *expected, size_t expected_size,
    const XrTargetInstructionRecord *rows, uint32_t count) {
    size_t encoded_size = 0;
    REQUIRE(xr_xtp_instruction_stream_size(rows, count, &encoded_size));
    REQUIRE(encoded_size == expected_size);
    uint8_t encoded[64] = {0};
    REQUIRE(encoded_size <= sizeof(encoded));
    REQUIRE(xr_xtp_instruction_stream_encode(rows, count, encoded,
                                             encoded_size, &encoded_size));
    REQUIRE(memcmp(encoded, expected, expected_size) == 0);
    XrTargetInstructionRecord decoded[2] = {0};
    REQUIRE(count <= 2u);
    REQUIRE(xr_xtp_instruction_stream_decode(encoded, encoded_size, count,
                                             decoded));
    for (uint32_t index = 0; index < count; index++) {
        uint8_t source_row[32] = {0};
        uint8_t decoded_row[32] = {0};
        REQUIRE(xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS,
                                   &rows[index], 1u, source_row));
        REQUIRE(xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS,
                                   &decoded[index], 1u, decoded_row));
        REQUIRE(memcmp(source_row, decoded_row, sizeof(source_row)) == 0);
    }
    uint8_t reencoded[64] = {0};
    size_t reencoded_size = 0;
    REQUIRE(xr_xtp_instruction_stream_encode(decoded, count, reencoded,
                                             sizeof(reencoded),
                                             &reencoded_size));
    REQUIRE(reencoded_size == expected_size &&
            memcmp(reencoded, expected, expected_size) == 0);
}

static void test_compact_instruction_stream_kat_and_mutations(void) {
    static const uint8_t const_return[] = {0x01, 0x00, 0x03, 0x01};
    static const uint8_t const_return_digest[XR_FINGERPRINT_BYTES] = {
        0x42, 0xd1, 0xb1, 0x57, 0x31, 0x20, 0x1a, 0xfb,
        0xc1, 0x21, 0x3b, 0x05, 0x73, 0x2b, 0xdc, 0xd8,
        0x54, 0x80, 0x41, 0x97, 0x75, 0x85, 0x82, 0x3f,
        0x8e, 0xf5, 0x88, 0xfc, 0x97, 0xbf, 0x07, 0xaa,
    };
    XrTargetInstructionRecord rows[2] = {
        {.id = 0,
         .function = 0,
         .result_slot = 2,
         .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                           XR_TARGET_INSTRUCTION_SLOT_NONE},
         .immediate_bits = UINT64_MAX,
         .opcode = XR_TARGET_INSTRUCTION_CONST_I64,
         .operand_count = 0,
         .reserved = 0},
        {.id = 1,
         .function = 0,
         .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
         .operand_slots = {2, XR_TARGET_INSTRUCTION_SLOT_NONE},
         .immediate_bits = 0,
         .opcode = XR_TARGET_INSTRUCTION_RETURN_I64,
         .operand_count = 1,
         .reserved = 0},
    };
    require_instruction_stream_reencodes(const_return,
                                         sizeof(const_return), rows, 2u);
    uint8_t digest[XR_FINGERPRINT_BYTES] = {0};
    xr_sha256(const_return, sizeof(const_return), digest);
    REQUIRE(memcmp(digest, const_return_digest, sizeof(digest)) == 0);

    static const uint8_t expanded_fingerprint[XR_FINGERPRINT_BYTES] = {
        0x3b, 0x9c, 0x9a, 0xb5, 0xa4, 0x77, 0xa8, 0xcd,
        0xb7, 0xc1, 0x0f, 0x1e, 0x78, 0xea, 0x9e, 0x4c,
        0xa8, 0x8a, 0xaf, 0xa1, 0xe9, 0x9f, 0x8b, 0x52,
        0xe5, 0xe6, 0x7d, 0x8a, 0x50, 0x4e, 0x0f, 0xdd,
    };
    static const uint8_t expanded_domain[] =
        "xray-xtp-expanded-instruction-kat-v1";
    uint8_t canonical_rows[64] = {0};
    REQUIRE(xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS, rows, 2u,
                               canonical_rows));
    XrSHA256Context fingerprint_context;
    xr_sha256_init(&fingerprint_context);
    xr_sha256_update(&fingerprint_context, expanded_domain,
                     sizeof(expanded_domain) - 1u);
    xr_sha256_update(&fingerprint_context, canonical_rows,
                     sizeof(canonical_rows));
    xr_sha256_final(&fingerprint_context, digest);
    REQUIRE(memcmp(digest, expanded_fingerprint, sizeof(digest)) == 0);

    static const uint8_t primitive_add[] = {0x00, 0x03, 0x00,
                                            0x03, 0x01, 0x02};
    XrTargetInstructionRecord add = {
        .id = 0,
        .function = 0,
        .result_slot = 2,
        .operand_slots = {0, 1},
        .immediate_bits = 0,
        .opcode = XR_TARGET_INSTRUCTION_ADD_WRAP_I64,
        .operand_count = 2,
        .reserved = 0,
    };
    require_instruction_stream_reencodes(primitive_add,
                                         sizeof(primitive_add), &add, 1u);

    static const struct {
        uint16_t first_opcode;
        uint64_t immediate;
        uint8_t bytes[4];
    } super_kats[] = {
        {XR_TARGET_INSTRUCTION_PARAM_I64, 0, {0x02, 0x00, 0x03, 0x00}},
        {XR_TARGET_INSTRUCTION_CALL_DIRECT_I64, 7, {0x03, 0x00, 0x03, 0x07}},
    };
    for (size_t index = 0; index < sizeof(super_kats) / sizeof(super_kats[0]);
         index++) {
        rows[0].opcode = super_kats[index].first_opcode;
        rows[0].immediate_bits = super_kats[index].immediate;
        require_instruction_stream_reencodes(super_kats[index].bytes,
                                             sizeof(super_kats[index].bytes),
                                             rows, 2u);
    }

    uint64_t work = 0;
    for (size_t length = 0; length < sizeof(const_return); length++)
        REQUIRE(!xr_xtp_instruction_stream_validate(
            const_return, length, 2u, &work));
    REQUIRE(!xr_xtp_instruction_stream_validate(
        const_return, sizeof(const_return), 1u, &work));
    uint8_t trailing[] = {0x01, 0x00, 0x03, 0x01, 0x00};
    REQUIRE(!xr_xtp_instruction_stream_validate(trailing, sizeof(trailing),
                                                2u, &work));
    uint8_t unknown[] = {0x7f};
    REQUIRE(!xr_xtp_instruction_stream_validate(unknown, sizeof(unknown), 1u,
                                                &work));
    uint8_t overlong[] = {0x00, 0x81, 0x00, 0x00};
    REQUIRE(!xr_xtp_instruction_stream_validate(overlong, sizeof(overlong), 1u,
                                                &work));
    uint8_t overflow[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0xff, 0xff, 0xff, 0x02};
    REQUIRE(!xr_xtp_instruction_stream_validate(overflow, sizeof(overflow),
                                                1u, &work));
    uint8_t no_slot_alias[] = {0x01, 0x00, 0x80, 0x80,
                               0x80, 0x80, 0x10, 0x00};
    REQUIRE(!xr_xtp_instruction_stream_validate(
        no_slot_alias, sizeof(no_slot_alias), 2u, &work));
    uint8_t primitive_pair[] = {0x00, 0x01, 0x00, 0x03, 0x01,
                                0x00, 0x0f, 0x00, 0x03};
    REQUIRE(!xr_xtp_instruction_stream_validate(
        primitive_pair, sizeof(primitive_pair), 2u, &work));

    XtpFixture fixture = make_direct_call_fixture();
    uint8_t *instruction_entry =
        directory_entry(fixture.bytes, XR_XTP_SECTION_INSTRUCTIONS);
    size_t instruction_offset =
        (size_t) xr_xtp_take_u64(instruction_entry + 8);
    size_t instruction_length =
        (size_t) xr_xtp_take_u64(instruction_entry + 16);
    REQUIRE(instruction_length > 0);
    for (size_t offset = 0; offset < instruction_length; offset++) {
        uint8_t *copy = copy_artifact(&fixture);
        copy[instruction_offset + offset] ^= UINT8_C(1);
        resign_section(copy, XR_XTP_SECTION_INSTRUCTIONS);
        resign_artifact(copy, fixture.size);
        expect_decode_or_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_header_and_directory_mutations(void) {
    XtpFixture fixture = make_fixture();
    uint8_t *copy = copy_artifact(&fixture);
    copy[XR_XTP_FULL_DIGEST_OFFSET] ^= 1;
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(copy + 4, UINT32_C(8)); /* v8 is a hard-cutover negative. */
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 48, 0);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 296, XR_XTP_MAX_TOTAL_ROWS + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    expect_decode_failure(fixture.bytes, XR_XTP_MAX_ARTIFACT_SIZE + 1u);

    uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 8, UINT64_MAX);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_INSTRUCTIONS);
    xr_xtp_put_u32(entry + 4, 0);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_INSTRUCTIONS);
    xr_xtp_put_u32(entry + 32, 32);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 16, xr_xtp_take_u64(entry + 16) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 24, xr_xtp_take_u64(entry + 24) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 32, xr_xtp_take_u32(entry + 32) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 36, 8);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(directory_entry(copy, XR_XTP_SECTION_TARGET_PROFILE),
                   XR_XTP_SECTION_MACHINE_REPS);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    entry[40] ^= 1;
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static void test_identity_and_typed_mutations(void) {
    XtpFixture fixture = make_fixture();
    static const size_t identity_offsets[] = {72, 104, 136, 168, 200, 232, 264};
    for (size_t i = 0; i < sizeof(identity_offsets) / sizeof(identity_offsets[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        copy[identity_offsets[i]] ^= 1;
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    size_t function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 16] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 20] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    static const size_t slot_mutations[] = {0, 24, 28, 32, 50};
    for (size_t i = 0; i < sizeof(slot_mutations) / sizeof(slot_mutations[0]); i++) {
        copy = copy_artifact(&fixture);
        uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
        size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
        copy[slot_offset + slot_mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_SLOTS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    copy = copy_artifact(&fixture);
    uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
    size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
    uint32_t slot_count = (uint32_t) xr_xtp_take_u64(slot_entry + 24);
    REQUIRE(slot_count >= 2);
    uint32_t slot_size = xr_xtp_take_u32(slot_entry + 32);
    memcpy(copy + slot_offset + slot_size, copy + slot_offset, XR_STABLE_ID_BYTES);
    resign_section(copy, XR_XTP_SECTION_SLOTS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    xr_xtp_put_u64(copy + 312, xr_xtp_take_u64(copy + 312) + 1u);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    static const size_t capability_mutations[] = {4, 8, 10};
    for (size_t i = 0;
         i < sizeof(capability_mutations) / sizeof(capability_mutations[0]);
         i++) {
        copy = copy_artifact(&fixture);
        uint8_t *capability_entry =
            directory_entry(copy, XR_XTP_SECTION_CAPABILITIES);
        size_t capability_offset =
            (size_t) xr_xtp_take_u64(capability_entry + 8);
        REQUIRE(xr_xtp_take_u64(capability_entry + 24) == 2);
        copy[capability_offset + capability_mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CAPABILITIES);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    static const size_t machine_identity_offsets[] = {4, 6, 8, 10};
    for (size_t i = 0;
         i < sizeof(machine_identity_offsets) /
                 sizeof(machine_identity_offsets[0]);
         i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *profile_entry =
            directory_entry(copy, XR_XTP_SECTION_TARGET_PROFILE);
        size_t profile_offset =
            (size_t) xr_xtp_take_u64(profile_entry + 8);
        copy[profile_offset + machine_identity_offsets[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_TARGET_PROFILE);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static int write_fixture(const char *path) {
    XtpFixture fixture = make_fixture();
    FILE *file = fopen(path, "wb");
    if (!file) {
        dispose_fixture(&fixture);
        return 1;
    }
    bool written = fwrite(fixture.bytes, 1, fixture.size, file) == fixture.size;
    bool closed = fclose(file) == 0;
    dispose_fixture(&fixture);
    return written && closed ? 0 : 1;
}

static int write_runtime_artifacts(const char *xsm_path,
                                   const char *xtp_path) {
    XtpFixture fixture = make_fixture();
    uint8_t *xsm = NULL;
    size_t xsm_size = 0;
    char diagnostic[512] = {0};
    if (!xr_xsm_encode(fixture.semantic, &xsm, &xsm_size, diagnostic,
                       sizeof(diagnostic))) {
        dispose_fixture(&fixture);
        return 1;
    }
    FILE *xsm_file = fopen(xsm_path, "wb");
    bool xsm_written =
        xsm_file && fwrite(xsm, 1, xsm_size, xsm_file) == xsm_size;
    bool xsm_closed = xsm_file && fclose(xsm_file) == 0;
    FILE *xtp_file = fopen(xtp_path, "wb");
    bool xtp_written = xtp_file &&
                       fwrite(fixture.bytes, 1, fixture.size, xtp_file) ==
                           fixture.size;
    bool xtp_closed = xtp_file && fclose(xtp_file) == 0;
    xr_free(xsm);
    dispose_fixture(&fixture);
    return xsm_written && xsm_closed && xtp_written && xtp_closed ? 0 : 1;
}

static bool write_c_byte_array(FILE *file, const char *name,
                               const uint8_t *bytes, size_t size) {
    if (fprintf(file, "static const uint8_t %s[] = {\n", name) < 0)
        return false;
    for (size_t i = 0; i < size; i++) {
        if ((i % 12u) == 0 && fputs("    ", file) == EOF)
            return false;
        if (fprintf(file, "0x%02x%s", bytes[i],
                    i + 1u == size ? "" : ", ") < 0)
            return false;
        if ((i % 12u) == 11u || i + 1u == size) {
            if (fputc('\n', file) == EOF)
                return false;
        }
    }
    return fputs("};\n", file) != EOF;
}

static int write_runtime_fixture_header(const char *path) {
    XtpFixture fixture = make_fixture();
    XtpFixture exported =
        make_fixture_from_semantic(build_exported_semantic_plan());
    uint8_t *xsm = NULL;
    size_t xsm_size = 0;
    uint8_t *export_xsm = NULL;
    size_t export_xsm_size = 0;
    char diagnostic[512] = {0};
    if (!xr_xsm_encode(fixture.semantic, &xsm, &xsm_size, diagnostic,
                       sizeof(diagnostic)) ||
        !xr_xsm_encode(exported.semantic, &export_xsm, &export_xsm_size,
                       diagnostic, sizeof(diagnostic))) {
        xr_free(xsm);
        xr_free(export_xsm);
        dispose_fixture(&fixture);
        dispose_fixture(&exported);
        return 1;
    }
    FILE *file = fopen(path, "wb");
    bool written = file &&
                   fputs("#ifndef XR_RUNTIME_SCALAR_ARTIFACT_FIXTURE_H\n"
                         "#define XR_RUNTIME_SCALAR_ARTIFACT_FIXTURE_H\n"
                         "#include <stdint.h>\n",
                         file) != EOF &&
                   write_c_byte_array(file, "xr_runtime_scalar_xsm", xsm,
                                      xsm_size) &&
                    write_c_byte_array(file, "xr_runtime_scalar_xtp",
                                       fixture.bytes, fixture.size) &&
                    write_c_byte_array(file, "xr_runtime_export_xsm",
                                       export_xsm, export_xsm_size) &&
                    write_c_byte_array(file, "xr_runtime_export_xtp",
                                       exported.bytes, exported.size) &&
                    fputs("#endif  // XR_RUNTIME_SCALAR_ARTIFACT_FIXTURE_H\n",
                          file) != EOF;
    bool closed = file && fclose(file) == 0;
    xr_free(xsm);
    xr_free(export_xsm);
    dispose_fixture(&fixture);
    dispose_fixture(&exported);
    return written && closed ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_fixture(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--write-runtime-artifacts") == 0)
        return write_runtime_artifacts(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--write-runtime-header") == 0)
        return write_runtime_fixture_header(argv[2]);
    if (argc == 2 && strcmp(argv[1], "schema-46-cutover") == 0) {
        test_exact_roundtrip_and_owned_candidate();
        puts("XTP schema 46 cutover tests passed");
        return 0;
    }
    test_artifact_classifier();
    test_wire_row_inventory();
    test_every_typed_row_codec();
    test_compact_instruction_stream_kat_and_mutations();
    test_exact_roundtrip_and_owned_candidate();
    test_concurrent_candidate_materialization();
    test_direct_call_rows_roundtrip_and_mutate();
    test_channel_close_row_roundtrip_and_mutate();
    test_stringbuilder_row_roundtrip_and_mutate();
    test_cleanup_row_roundtrip_and_mutate();
    test_coroutine_lifecycle_rows_roundtrip_and_mutate();
    test_source_export_row_roundtrip_and_mutate();
    test_coroutine_rows_roundtrip_and_mutate();
    test_header_and_directory_mutations();
    test_identity_and_typed_mutations();
    test_runtime_machine_authority_is_exact_and_scalar_only();
    test_runtime_factory_owns_native_profile();
    test_assertion_artifact_capability_identity();
    test_public_xsm_authority_loads_exact_semantics();
    test_independent_verifier_rejects_forged_machine_profiles();
    test_runtime_load_rejects_foreign_profile_artifacts();
    test_runtime_load_materializes_only_verified_plan();
    puts("typed XTP format tests passed");
    return 0;
}
