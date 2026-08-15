/*
 * test_typed_opaque_boundary.c - Typed VM opaque-handle admission boundary
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_typed_frame.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, \
                    __LINE__, #condition);                                    \
            abort();                                                          \
        }                                                                     \
    } while (0)

typedef struct PlanFixture {
    XrSemanticPlan *semantic;
    XrSemanticPlan *dependency;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
} PlanFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 2,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_raw_pointer = {
    .kind = XR_KIND_POINTER,
    .id = 3,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_scalar_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .return_type = &stub_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_raw_pointer_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 10,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .return_type = &stub_raw_pointer,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_module_namespace = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 5,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_channel = {
    .kind = XR_KIND_CHANNEL,
    .id = 6,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_mutex = {
    .kind = XR_KIND_INSTANCE,
    .id = 7,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "Mutex"},
};
static XrType stub_socket = {
    .kind = XR_KIND_INSTANCE,
    .id = 8,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "Socket"},
};
static XrType stub_foreign_handle = {
    .kind = XR_KIND_INSTANCE,
    .id = 9,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "ForeignHandle"},
};
static XrType stub_graph_node = {
    .kind = XR_KIND_INSTANCE,
    .id = 11,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "GraphNode"},
};

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
    char diagnostic[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, diagnostic,
                                    sizeof(diagnostic)));
    return profile;
}

static XrSemanticPlan *build_identity_semantic(const char *name,
                                               XrType *type) {
    XiFunc *function = xi_func_new(name, type);
    REQUIRE(function);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry);
    entry->sealed = true;
    function->nparams = function->min_params = 1;
    function->params =
        (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params);
    function->params[0] = xi_param(function, entry, 0, type);
    REQUIRE(function->params[0]);
    xi_block_set_return(entry, function->params[0]);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, diagnostic,
                                        sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "%s semantic build failed: %s\n", name,
                diagnostic);
    REQUIRE(built && semantic);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_channel_semantic(void) {
    XiFunc *function = xi_func_new("opaque_channel", &stub_unit);
    REQUIRE(function);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry);
    XiValue *capacity = xi_const_int(function, entry, 1, &stub_int);
    XiValue *channel =
        xi_value_new(function, entry, XI_CHAN_NEW, &stub_channel, 1);
    XiValue *alias =
        xi_value_new(function, entry, XI_COPY, &stub_channel, 1);
    REQUIRE(capacity && channel && alias);
    channel->args[0] = capacity;
    alias->args[0] = channel;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, diagnostic,
                                   sizeof(diagnostic)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_raw_pointer_semantic(void) {
    XiFunc *root = xi_func_new("opaque_raw_pointer_root",
                               &stub_raw_pointer);
    XiFunc *child = xi_func_new("opaque_raw_pointer_child",
                                &stub_raw_pointer);
    REQUIRE(root && child);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry && child_entry);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params);
    child->params[0] =
        xi_param(child, child_entry, 0, &stub_raw_pointer);
    REQUIRE(child->params[0]);
    xi_block_set_return(child_entry, child->params[0]);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *callee = xi_value_new(root, root_entry, XI_STACK_ALLOC,
                                   &stub_raw_pointer_function, 0);
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY,
                                  &stub_raw_pointer_function, 1);
    XiValue *argument =
        xi_const_int(root, root_entry, 1, &stub_raw_pointer);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL,
                                 &stub_raw_pointer, 2);
    REQUIRE(callee && alias && argument && call);
    callee->aux_int = XI_CLOSURE_NEW;
    callee->aux = child;
    alias->args[0] = callee;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    call->args[0] = alias;
    call->args[1] = argument;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    bool built = xr_semantic_plan_build(root, &semantic, diagnostic,
                                        sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "raw-pointer semantic build failed: %s\n",
                diagnostic);
    REQUIRE(built && semantic &&
            xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static PlanFixture build_plan(XrSemanticPlan *semantic) {
    PlanFixture fixture = {.semantic = semantic, .profile = build_profile()};
    char diagnostic[512] = {0};
    bool built = xr_target_plan_build(semantic, fixture.profile,
                                      &fixture.plan, diagnostic,
                                      sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "target build failed: %s\n", diagnostic);
    REQUIRE(built && fixture.plan &&
            xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static void dispose_plan(PlanFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    xr_semantic_plan_free(fixture->dependency);
    memset(fixture, 0, sizeof(*fixture));
}

static uint32_t find_function_with_rep(const XrTargetPlan *plan,
                                       uint16_t kind,
                                       uint32_t *slot_out) {
    uint32_t function_count = 0;
    uint32_t slot_count = 0;
    uint32_t rep_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetMachineRepRecord *reps =
        xr_target_plan_machine_reps(plan, &rep_count);
    REQUIRE(functions && reps && (slot_count == 0 || slots));
    for (uint32_t function = 0; function < function_count; function++) {
        const XrTargetFunctionRecord *record = &functions[function];
        REQUIRE(record->slot_begin <= slot_count &&
                record->slot_count <= slot_count - record->slot_begin);
        for (uint32_t i = 0; i < record->slot_count; i++) {
            uint32_t slot = record->slot_begin + i;
            REQUIRE(slots[slot].register_rep < rep_count);
            if (reps[slots[slot].register_rep].kind != kind)
                continue;
            *slot_out = slot;
            return function;
        }
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static XrTypedFrameStatus create_frame(const XrTargetPlan *plan,
                                       uint32_t function,
                                       XrTypedFrame **frame) {
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    return xr_typed_frame_create(plan, &fingerprint, function, &limits,
                                 frame);
}

static void require_verify_rejected(XrTargetPlan *plan,
                                    const char *diagnostic_code) {
    char diagnostic[512] = {0};
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(plan, diagnostic, sizeof(diagnostic)));
    REQUIRE(strncmp(diagnostic, diagnostic_code,
                    strlen(diagnostic_code)) == 0);
}

static void test_raw_pointer_is_opaque_bytes(void) {
    PlanFixture fixture = build_plan(build_raw_pointer_semantic());
    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t function = find_function_with_rep(
        fixture.plan, XR_MACHINE_REP_RAW_PTR, &slot);
    REQUIRE(function != XR_SEMANTIC_INDEX_NONE &&
            slot != XR_SEMANTIC_INDEX_NONE);
    const XrTargetSlotRecord *slot_record = &fixture.plan->slots[slot];
    XrTargetMachineRepRecord *rep =
        &fixture.plan->machine_reps[slot_record->register_rep];
    REQUIRE(rep->kind == XR_MACHINE_REP_RAW_PTR &&
            rep->root_kind == XR_TARGET_ROOT_NONE &&
            rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
            rep->null_encoding == XR_TARGET_NULL_ZERO &&
            rep->memory_size == sizeof(uintptr_t));

    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(fixture.plan, function, &frame) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(frame);
    XrTypedSlotAccess access = {0};
    REQUIRE(xr_typed_frame_describe_slot(frame, slot, &access) ==
            XR_TYPED_FRAME_OK);
    uintptr_t invalid_address = (uintptr_t) 1u;
    uintptr_t roundtrip = 0;
    REQUIRE(access.size == sizeof(invalid_address));
    REQUIRE(xr_typed_frame_store(frame, &access, &invalid_address,
                                 sizeof(invalid_address)) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_load(frame, &access, &roundtrip,
                                sizeof(roundtrip)) == XR_TYPED_FRAME_OK);
    REQUIRE(roundtrip == invalid_address);
    REQUIRE(xr_typed_frame_free(&frame) == XR_TYPED_FRAME_OK && !frame);

    XrTargetMachineRepRecord saved = *rep;
    rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    require_verify_rejected(fixture.plan, "XR_TARGET_1001");
    XrTypedFrame *rejected = NULL;
    REQUIRE(create_frame(fixture.plan, function, &rejected) ==
            XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!rejected);

    *rep = saved;
    rep->root_kind = XR_TARGET_ROOT_OBJECT;
    rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    require_verify_rejected(fixture.plan, "XR_TARGET_1001");
    REQUIRE(create_frame(fixture.plan, function, &rejected) ==
            XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!rejected);

    *rep = saved;
    xr_target_plan_compute_fingerprint(fixture.plan,
                                       &fixture.plan->fingerprint);
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic,
                                  sizeof(diagnostic)));
    dispose_plan(&fixture);
}

static void require_rooted_object_execution_unavailable(XrType *type,
                                                        const char *name) {
    XrSemanticPlan *semantic = build_identity_semantic(name, type);
    bool rooted_object_type = false;
    for (uint32_t i = 0; i < semantic->type_count; i++) {
        const XrSemanticTypeRecord *record = &semantic->types[i];
        if (record->kind == XR_KIND_INSTANCE &&
            (record->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                              XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
                (XR_SEM_TYPE_REFERENCE_CAPABLE |
                 XR_SEM_TYPE_OWNERSHIP_ROOT))
            rooted_object_type = true;
    }
    REQUIRE(rooted_object_type);
    XrTargetProfile *profile = build_profile();
    XrTargetPlan *plan = NULL;
    char diagnostic[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, diagnostic,
                                      sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "%s target build failed: %s\n", name, diagnostic);
    REQUIRE(built && plan && xr_target_plan_is_verified(plan));
    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t function = find_function_with_rep(
        plan, XR_MACHINE_REP_DYN_VALUE, &slot);
    REQUIRE(function == XR_SEMANTIC_INDEX_NONE &&
            slot == XR_SEMANTIC_INDEX_NONE);
    uint32_t function_count = 0;
    REQUIRE(xr_target_plan_functions(plan, &function_count));
    for (uint32_t i = 0; i < function_count; i++)
        REQUIRE(xr_target_plan_function_execution_family_mask(plan, i) == 0);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_rooted_handles_are_not_frame_transport(void) {
    PlanFixture channel = build_plan(build_channel_semantic());
    uint32_t channel_slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t channel_function = find_function_with_rep(
        channel.plan, XR_MACHINE_REP_DYN_VALUE, &channel_slot);
    REQUIRE(channel_function != XR_SEMANTIC_INDEX_NONE &&
            channel_slot != XR_SEMANTIC_INDEX_NONE);
    const XrTargetMachineRepRecord *channel_rep =
        &channel.plan->machine_reps[
            channel.plan->slots[channel_slot].register_rep];
    REQUIRE(channel_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            channel_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL);
    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(channel.plan, channel_function, &frame) ==
            XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!frame);
    dispose_plan(&channel);

    require_rooted_object_execution_unavailable(&stub_mutex,
                                                "opaque_mutex");
    require_rooted_object_execution_unavailable(&stub_socket,
                                                "opaque_socket");
    require_rooted_object_execution_unavailable(
        &stub_foreign_handle, "opaque_foreign_handle");
    require_rooted_object_execution_unavailable(&stub_graph_node,
                                                "opaque_graph_node");
}

static void test_fabricated_adapters_fail_closed(void) {
    PlanFixture fixture = build_plan(build_raw_pointer_semantic());
    REQUIRE(fixture.plan->adapters_count == 0 &&
            fixture.plan->adapters == NULL);
    static const uint8_t kinds[] = {
        XR_TARGET_ADAPTER_BOX_DYNAMIC,
        XR_TARGET_ADAPTER_UNBOX_DYNAMIC,
        XR_TARGET_ADAPTER_FFI,
        XR_TARGET_ADAPTER_HOSTED,
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        XrTargetAdapterRecord fabricated = {.id = 0, .kind = kinds[i]};
        fixture.plan->adapters = &fabricated;
        fixture.plan->adapters_count = 1;
        require_verify_rejected(fixture.plan, "XR_TARGET_1003");
        fixture.plan->adapters = NULL;
        fixture.plan->adapters_count = 0;
        xr_target_plan_compute_fingerprint(fixture.plan,
                                           &fixture.plan->fingerprint);
    }
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic,
                                  sizeof(diagnostic)));
    dispose_plan(&fixture);
}

typedef struct EntryResolver {
    const XiFunc *callee;
} EntryResolver;

static const XiFunc *resolve_entry(void *context, const XiFunc *current,
                                   const XiValue *call) {
    (void) current;
    EntryResolver *resolver = (EntryResolver *) context;
    return resolver && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "echo") == 0
               ? resolver->callee
               : NULL;
}

static int entry_suspendability(void *context, const XiFunc *current,
                                const XiValue *call) {
    (void) context;
    (void) current;
    return call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "echo") == 0
               ? 0
               : -1;
}

static PlanFixture build_entry_plan(void) {
    XiFunc *dependency_root = xi_func_new("opaque_provider", &stub_unit);
    XiFunc *echo = xi_func_new("echo", &stub_int);
    REQUIRE(dependency_root && echo);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *echo_entry = xi_block_new(echo);
    REQUIRE(dependency_entry && echo_entry);
    dependency_entry->sealed = echo_entry->sealed = true;
    dependency_root->children =
        (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = echo;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    echo->parent_func = dependency_root;
    echo->nparams = echo->min_params = 1;
    echo->params = (XiValue **) xr_calloc(1, sizeof(*echo->params));
    REQUIRE(echo->params);
    echo->params[0] = xi_param(echo, echo_entry, 0, &stub_int);
    REQUIRE(echo->params[0]);
    XiValue *closure = xi_value_new(dependency_root, dependency_entry,
                                    XI_CLOSURE_NEW,
                                    &stub_scalar_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry,
                                  XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(closure && store);
    closure->aux = echo;
    store->args[0] = closure;
    store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    xi_block_set_return(echo_entry, echo->params[0]);
    dependency_root->stage = echo->stage = XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = echo->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = echo->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module = xi_module_new(
        "test/opaque/provider.xr", "opaque_provider", dependency_root);
    REQUIRE(dependency_module);
    dependency_root->module = dependency_module;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports);
    dependency_module->exports[0] = (XiModuleExport) {
        .name = "echo",
        .shared_slot = 0,
        .function = echo,
    };
    char diagnostic[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(
        dependency_root, diagnostic, sizeof(diagnostic)));
    XrSemanticPlan *dependency =
        xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency &&
            xr_semantic_plan_source_export_count(dependency) == 1);

    XiFunc *caller_root = xi_func_new("opaque_consumer", &stub_unit);
    XiFunc *caller = xi_func_new("call_echo", &stub_int);
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
    XiImportRef import = {
        .module_path = "test/opaque/provider.xr",
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = dependency_module,
    };
    XiValue *namespace_ref = xi_value_new(
        caller_root, root_entry, XI_IMPORT_REF, &stub_module_namespace, 0);
    XiValue *namespace_alias = xi_value_new(
        caller_root, root_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *namespace_store = xi_value_new(
        caller_root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(namespace_ref && namespace_alias && namespace_store);
    namespace_ref->aux = &import;
    namespace_alias->args[0] = namespace_ref;
    namespace_alias->aux_int = XI_COPY_KIND_IDENTITY;
    namespace_store->args[0] = namespace_alias;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver = xi_value_new(caller, caller_entry, XI_GET_SHARED,
                                     &stub_module_namespace, 0);
    XiValue *receiver_alias = xi_value_new(
        caller, caller_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *argument = xi_const_int(caller, caller_entry, 41, &stub_int);
    XiValue *method = xi_value_new(caller, caller_entry, XI_CALL_METHOD,
                                   &stub_int, 2);
    REQUIRE(receiver && receiver_alias && argument && method);
    receiver->aux_int = 0;
    receiver_alias->args[0] = receiver;
    receiver_alias->aux_int = XI_COPY_KIND_IDENTITY;
    method->args[0] = receiver_alias;
    method->args[1] = argument;
    method->aux = (void *) "echo";
    method->aux_int = 0;
    xi_block_set_return(caller_entry, method);
    caller_root->stage = caller->stage = XI_STAGE_SEMANTIC_LOWERED;
    caller_root->invariant_mask = caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    EntryResolver resolver_state = {.callee = echo};
    XiCoroResolver resolver = {
        .resolve_method = resolve_entry,
        .call_suspendability = entry_suspendability,
        .ud = &resolver_state,
    };
    REQUIRE(xi_coro_lower(caller_root, &resolver));
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module = xi_module_new(
        "test/opaque/consumer.xr", "opaque_consumer", caller_root);
    REQUIRE(caller_module);
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependencies[] = {dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(
        caller_root, dependencies, 1, diagnostic, sizeof(diagnostic)));
    XrSemanticPlan *semantic =
        xr_semantic_plan_retain(caller_root->semantic_plan);
    REQUIRE(semantic);

    PlanFixture fixture = {
        .semantic = semantic,
        .dependency = dependency,
        .profile = build_profile(),
    };
    const XrSemanticPlan *semantic_dependencies[] = {dependency};
    REQUIRE(xr_target_plan_build_module_set(
        semantic, semantic_dependencies, 1, fixture.profile, &fixture.plan,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(fixture.plan && fixture.plan->entry_expectations_count == 1 &&
            fixture.plan->entry_expectations[0].adapter_kind ==
                XR_TARGET_ENTRY_ADAPTER_IDENTITY);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
    return fixture;
}

static void test_non_identity_entry_rejected(void) {
    PlanFixture fixture = build_entry_plan();
    fixture.plan->entry_expectations[0].adapter_kind =
        XR_TARGET_ENTRY_ADAPTER_INVALID;
    require_verify_rejected(fixture.plan, "XR_TARGET_1005");
    fixture.plan->entry_expectations[0].adapter_kind =
        XR_TARGET_ENTRY_ADAPTER_IDENTITY;
    xr_target_plan_compute_fingerprint(fixture.plan,
                                       &fixture.plan->fingerprint);
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic,
                                  sizeof(diagnostic)));
    dispose_plan(&fixture);
}

int main(void) {
    test_raw_pointer_is_opaque_bytes();
    test_rooted_handles_are_not_frame_transport();
    test_fabricated_adapters_fail_closed();
    test_non_identity_entry_rejected();
    puts("typed opaque boundary tests passed");
    return 0;
}
