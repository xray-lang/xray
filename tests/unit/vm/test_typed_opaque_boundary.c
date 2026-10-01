/*
 * test_typed_opaque_boundary.c - Typed VM opaque-handle admission boundary
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/abi/xr_runtime_target_profile.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/stdlib/xstdlib_metadata.h"
#include "../../../src/vm/xr_typed_frame.h"
#include "../../../src/vm/xr_typed_lifecycle.h"
#include "../../../src/vm/xr_typed_dispatch.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
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
    .function =
        {
            .return_type = &stub_int,
            .throw_effect = XR_FN_EFFECT_NO_THROW,
        },
};
static XrType stub_raw_pointer_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 10,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function =
        {
            .return_type = &stub_raw_pointer,
            .throw_effect = XR_FN_EFFECT_NO_THROW,
        },
};
static XrType stub_module_namespace = {
    /* Legacy SemanticPlan namespaces use an exact borrowed-static import
     * carrier, not the inline structural value kind. */
    .kind = XR_KIND_UNKNOWN,
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
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) == XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization = &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, diagnostic, sizeof(diagnostic)));
    return profile;
}

/* The SemanticPlan builder requires a lowered graph to carry a typed durable
 * module identity and synthesizes none, so each fixture names its own
 * memory-namespace identity. xi_func_free owns the module it is attached to
 * and releases it with the function. */
static void attach_fixture_module(XiFunc *root, const char *name) {
    char identity[256];
    int written =
        snprintf(identity, sizeof(identity), "memory-module-v1:id=%zu:%s", strlen(name), name);
    REQUIRE(written > 0 && (size_t) written < sizeof(identity));
    if (!root->module) {
        root->module = xi_module_new("test/opaque/fixture.xr", "opaque_fixture", root);
        REQUIRE(root->module);
    }
    /* xi_module_new leaves the identity unset, so a fixture that built its own
     * module still has to name one. */
    if (!root->module->identity)
        REQUIRE(xi_module_set_identity(root->module, identity));
}

static XrSemanticPlan *build_identity_semantic(const char *name, XrType *type,
                                                 XiClassData *source_class, bool borrowed) {
    XiFunc *function = xi_func_new(name, type);
    REQUIRE(function);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry);
    entry->sealed = true;
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params);
    function->params[0] = xi_param(function, entry, 0, type);
    REQUIRE(function->params[0]);
    function->params[0]->param_mode = XR_PARAM_READ;
    function->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    if (borrowed) {
        function->arc_borrow_sig =
            (XiBorrowSig *) xi_func_arena_alloc(function, sizeof(*function->arc_borrow_sig));
        REQUIRE(function->arc_borrow_sig);
        memset(function->arc_borrow_sig, 0, sizeof(*function->arc_borrow_sig));
        function->arc_borrow_sig->nparams = 1;
        function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
        function->arc_borrow_sig->valid = true;
    }
    xi_block_set_return(entry, function->params[0]);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    attach_fixture_module(function, "opaque-identity-fixture");
    if (source_class) {
        function->module->classes = (XiClassData **) xr_calloc(1, sizeof(*function->module->classes));
        REQUIRE(function->module->classes);
        function->module->classes[0] = source_class;
        function->module->nclasses = 1;
    }
    bool built = xr_semantic_plan_build(function, &semantic, diagnostic, sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "%s semantic build failed: %s\n", name, diagnostic);
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
    XiValue *channel = xi_value_new(function, entry, XI_CHAN_NEW, &stub_channel, 1);
    XiValue *alias = xi_value_new(function, entry, XI_COPY, &stub_channel, 1);
    REQUIRE(capacity && channel && alias);
    channel->args[0] = capacity;
    alias->args[0] = channel;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    attach_fixture_module(function, "opaque-channel-fixture");
    REQUIRE(xr_semantic_plan_build(function, &semantic, diagnostic, sizeof(diagnostic)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_raw_pointer_semantic(void) {
    XiFunc *root = xi_func_new("opaque_raw_pointer_root", &stub_raw_pointer);
    XiFunc *child = xi_func_new("opaque_raw_pointer_child", &stub_raw_pointer);
    REQUIRE(root && child);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry && child_entry);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params);
    child->params[0] = xi_param(child, child_entry, 0, &stub_raw_pointer);
    REQUIRE(child->params[0]);
    xi_block_set_return(child_entry, child->params[0]);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *callee = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_raw_pointer_function, 0);
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_raw_pointer_function, 1);
    XiValue *argument = xi_const_int(root, root_entry, 1, &stub_raw_pointer);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_raw_pointer, 2);
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
    attach_fixture_module(root, "opaque-raw-pointer-fixture");
    bool built = xr_semantic_plan_build(root, &semantic, diagnostic, sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "raw-pointer semantic build failed: %s\n", diagnostic);
    REQUIRE(built && semantic && xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static PlanFixture build_plan(XrSemanticPlan *semantic) {
    PlanFixture fixture = {.semantic = semantic, .profile = build_profile()};
    char diagnostic[512] = {0};
    bool built = xr_target_plan_build(semantic, fixture.profile, &fixture.plan, diagnostic,
                                      sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "target build failed: %s\n", diagnostic);
    REQUIRE(built && fixture.plan && xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static void dispose_plan(PlanFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    xr_semantic_plan_free(fixture->dependency);
    memset(fixture, 0, sizeof(*fixture));
}

static uint32_t find_function_with_rep(const XrTargetPlan *plan, uint16_t kind,
                                       uint32_t *slot_out) {
    uint32_t function_count = 0;
    uint32_t slot_count = 0;
    uint32_t rep_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetMachineRepRecord *reps = xr_target_plan_machine_reps(plan, &rep_count);
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

static XrTypedFrameStatus create_frame(const XrTargetPlan *plan, uint32_t function,
                                       XrTypedFrame **frame) {
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    return xr_typed_frame_create(plan, &fingerprint, function, &limits, frame);
}

static void require_verify_rejected(XrTargetPlan *plan, const char *diagnostic_code) {
    char diagnostic[512] = {0};
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(plan, diagnostic, sizeof(diagnostic)));
    REQUIRE(strncmp(diagnostic, diagnostic_code, strlen(diagnostic_code)) == 0);
}

static void test_raw_pointer_is_opaque_bytes(void) {
    PlanFixture fixture = build_plan(build_raw_pointer_semantic());
    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t function = find_function_with_rep(fixture.plan, XR_MACHINE_REP_RAW_PTR, &slot);
    REQUIRE(function != XR_SEMANTIC_INDEX_NONE && slot != XR_SEMANTIC_INDEX_NONE);
    const XrTargetSlotRecord *slot_record = &fixture.plan->slots[slot];
    XrTargetMachineRepRecord *rep = &fixture.plan->machine_reps[slot_record->register_rep];
    REQUIRE(rep->kind == XR_MACHINE_REP_RAW_PTR && rep->root_kind == XR_TARGET_ROOT_NONE &&
            rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
            rep->null_encoding == XR_TARGET_NULL_ZERO && rep->memory_size == sizeof(uintptr_t));

    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(fixture.plan, function, &frame) == XR_TYPED_FRAME_OK);
    REQUIRE(frame);
    XrTypedSlotAccess access = {0};
    REQUIRE(xr_typed_frame_describe_slot(frame, slot, &access) == XR_TYPED_FRAME_OK);
    uintptr_t invalid_address = (uintptr_t) 1u;
    uintptr_t roundtrip = 0;
    REQUIRE(access.size == sizeof(invalid_address));
    REQUIRE(xr_typed_frame_store(frame, &access, &invalid_address, sizeof(invalid_address)) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_load(frame, &access, &roundtrip, sizeof(roundtrip)) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(roundtrip == invalid_address);
    REQUIRE(xr_typed_frame_free(&frame) == XR_TYPED_FRAME_OK && !frame);

    XrTargetMachineRepRecord saved = *rep;
    rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    require_verify_rejected(fixture.plan, "XR_TARGET_1001");
    XrTypedFrame *rejected = NULL;
    REQUIRE(create_frame(fixture.plan, function, &rejected) == XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!rejected);

    *rep = saved;
    rep->root_kind = XR_TARGET_ROOT_OBJECT;
    rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    require_verify_rejected(fixture.plan, "XR_TARGET_1001");
    REQUIRE(create_frame(fixture.plan, function, &rejected) == XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!rejected);

    *rep = saved;
    xr_target_plan_compute_fingerprint(fixture.plan, &fixture.plan->fingerprint);
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic, sizeof(diagnostic)));
    dispose_plan(&fixture);
}

typedef struct RejectedOwnerProbe {
    uint32_t resolves;
    uint32_t reclaims;
} RejectedOwnerProbe;

static uint32_t rejected_kernel_calls;

static XrRuntimeObjectHeader *reject_object_resolution(void *context, uintptr_t address) {
    RejectedOwnerProbe *probe = (RejectedOwnerProbe *) context;
    (void) address;
    probe->resolves++;
    return NULL;
}

static void reject_object_reclamation(void *context, XrRuntimeObjectHeader *header) {
    RejectedOwnerProbe *probe = (RejectedOwnerProbe *) context;
    (void) header;
    probe->reclaims++;
}

static XrArrayPushStatus reject_array_push(XrValue receiver, XrValue value) {
    (void) receiver;
    (void) value;
    rejected_kernel_calls++;
    return XR_ARRAY_PUSH_INVALID_ARRAY;
}

/* A carrier does not authorize a RELEASE or a native kernel. Refusal must
 * preserve the caller's bytes and must never ask an allocator to resolve an
 * opaque address. Both generated dispatch providers enforce the same edge. */
static void require_execution_authority_unavailable(const XrTargetPlan *plan, uint32_t function) {
    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(plan, function, &frame) == XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!frame);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    RejectedOwnerProbe probe = {0};
    XrTypedLifecycleBindings bindings = {
        .resolve_object = reject_object_resolution,
        .reclaim_object = reject_object_reclamation,
        .allocation_context = &probe,
    };
    XrTypedLifecycleContext lifecycle = {0};
    REQUIRE(xr_typed_lifecycle_context_init(plan, &fingerprint, function, &bindings, &lifecycle) ==
            XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE);
    REQUIRE(!lifecycle.plan && !lifecycle.owners && lifecycle.owner_count == 0);
    xr_typed_lifecycle_context_dispose(&lifecycle);
    REQUIRE(probe.resolves == 0 && probe.reclaims == 0);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        int64_t argument = 43;
        int64_t result = 9;
        XrTypedDispatchI64Request scalar_request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &fingerprint,
            .arguments = &argument,
            .result = &result,
            .provider = providers[i],
            .function = function,
            .argument_count = 1,
        };
        REQUIRE(xr_typed_dispatch_execute_i64(&scalar_request) == XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
        REQUIRE(argument == 43 && result == 0);
        XrValue arguments[2] = {
            {.tag = XR_TAG_PTR, .ptr = (void *) (uintptr_t) 1u, .heap_type = 0},
            {.tag = XR_TAG_I64, .i = 73},
        };
        XrValue saved[2];
        memcpy(saved, arguments, sizeof(saved));
        rejected_kernel_calls = 0;
        XrTypedDispatchValueRequest request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &fingerprint,
            .arguments = arguments,
            .array_push = reject_array_push,
            .provider = providers[i],
            .function = function,
            .argument_count = 2,
        };
        REQUIRE(xr_typed_dispatch_execute_values(&request) == XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
        REQUIRE(memcmp(saved, arguments, sizeof(saved)) == 0 && rejected_kernel_calls == 0);
    }
}

static void require_rooted_object_execution_unavailable(XrType *type, const char *name,
                                                        XiClassData *declaration, bool borrowed) {
    PlanFixture fixture = build_plan(build_identity_semantic(name, type, declaration, borrowed));
    XrSemanticPlan *semantic = fixture.semantic;
    REQUIRE(xr_semantic_plan_is_verified(semantic) && semantic->function_count == 1 &&
            semantic->parameter_count == 1 && semantic->call_target_count == 0);
    const XrSemanticParameterRecord *parameter = &semantic->parameters[0];
    REQUIRE(parameter->function == 0 && parameter->ordinal == 0 &&
            parameter->mode == XR_PARAM_READ && parameter->transfer_mode == XR_TRANSFER_SHARE &&
            parameter->flags == XR_SEM_PARAMETER_REQUIRED &&
            parameter->ownership == (borrowed ? XI_OWN_BORROWED : XI_OWN_OWNED));
    REQUIRE(parameter->type < semantic->type_count &&
            semantic->functions[0].return_type == parameter->type);
    const XrSemanticTypeRecord *record = &semantic->types[parameter->type];
    REQUIRE(record->kind == XR_KIND_INSTANCE && record->builtin_type == XR_TID_NULL &&
            record->scalar_rep == XR_SCALAR_REP_NONE && record->child_count == 0 &&
            record->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT));
    XrStableId zero = {{0}};
    if (declaration) {
        REQUIRE(semantic->source_class_count == 1 && record->source_class == 0 &&
                !xr_stable_id_equal(record->source_class_identity, zero));
        const XrSemanticSourceClassRecord *source_class = &semantic->source_classes[0];
        REQUIRE(xr_stable_id_equal(source_class->id, record->source_class_identity) &&
                strcmp(source_class->name, type->instance.class_name) == 0 &&
                source_class->ordinal == 0 && source_class->method_count == 0 &&
                source_class->flags ==
                    (XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL | XR_SEM_SOURCE_CLASS_RUNTIME_TYPE) &&
                strstr(record->canonical_key, ";source-class:") != NULL);
    } else {
        REQUIRE(semantic->source_class_count == 0 && record->source_class == XR_SEMANTIC_INDEX_NONE &&
                xr_stable_id_equal(record->source_class_identity, zero) &&
                strstr(record->canonical_key, ";source-class:") == NULL);
    }
    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t function = find_function_with_rep(fixture.plan, XR_MACHINE_REP_DYN_VALUE, &slot);
    REQUIRE(function == 0 && slot != XR_SEMANTIC_INDEX_NONE);
    const XrTargetSlotRecord *slot_record = &fixture.plan->slots[slot];
    const XrTargetMachineRepRecord *rep = &fixture.plan->machine_reps[slot_record->register_rep];
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(fixture.profile);
    uint8_t expected_ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED;
    REQUIRE(slot_record->semantic_value == parameter->value &&
            slot_record->semantic_operation == XR_SEMANTIC_INDEX_NONE &&
            slot_record->logical_slot == XR_SEMANTIC_INDEX_NONE &&
            slot_record->role == XR_TARGET_SLOT_PARAMETER &&
            slot_record->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            slot_record->ownership == expected_ownership &&
            slot_record->register_rep == slot_record->memory_rep &&
            rep->kind == XR_MACHINE_REP_DYN_VALUE && rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            rep->ownership == expected_ownership && rep->null_encoding == XR_TARGET_NULL_TAGGED &&
            rep->register_bits == facts->data_layout.xr_value.size * 8u &&
            slot_record->size == facts->data_layout.xr_value.size &&
            slot_record->align == facts->data_layout.xr_value.align &&
            rep->memory_size == slot_record->size && rep->memory_align == slot_record->align);
    REQUIRE(fixture.plan->functions_count == 1 && fixture.plan->slots_count == 1 &&
            fixture.plan->instructions_count == 0 && fixture.plan->calls_count == 0 &&
            fixture.plan->call_arguments_count == 0 && fixture.plan->root_maps_count == 0 &&
            fixture.plan->root_slots_count == 0 && fixture.plan->cleanups_count == 0 &&
            fixture.plan->coroutines_count == 0 && fixture.plan->adapters_count == 0 &&
            fixture.plan->entry_expectations_count == 0);
    REQUIRE(xr_target_plan_function_execution_family_mask(fixture.plan, function) == 0 &&
            fixture.plan->functions[function].root_count == 0 &&
            fixture.plan->functions[function].cleanup_count == 0);
    /* Baseline allocator/panic capabilities do not identify a native callee,
     * an opaque payload adapter, an owner root or a field/provider binding. */
    REQUIRE(fixture.plan->capabilities_count == 2 &&
            fixture.plan->capabilities[0].capability == XR_TARGET_CAPABILITY_ALLOCATOR &&
            fixture.plan->capabilities[0].provider_role == XR_TARGET_PROVIDER_ROLE_ALLOCATOR &&
            fixture.plan->capabilities[1].capability == XR_TARGET_CAPABILITY_PANIC &&
            fixture.plan->capabilities[1].provider_role == XR_TARGET_PROVIDER_ROLE_PANIC);
    require_execution_authority_unavailable(fixture.plan, function);
    printf("opaque %s: source_classes=%u ownership=%s DYN=1 instructions=0 roots=0 cleanup=0 "
           "calls=0 frame=SLOT_INVALID lifecycle=CONTRACT_UNAVAILABLE providers=2 scalar/value refused\n",
           name, semantic->source_class_count, borrowed ? "borrowed" : "owned");
    fflush(stdout);
    dispose_plan(&fixture);
}

static void test_rooted_handles_are_not_frame_transport(void) {
    PlanFixture channel = build_plan(build_channel_semantic());
    uint32_t channel_slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t channel_function =
        find_function_with_rep(channel.plan, XR_MACHINE_REP_DYN_VALUE, &channel_slot);
    REQUIRE(channel_function != XR_SEMANTIC_INDEX_NONE && channel_slot != XR_SEMANTIC_INDEX_NONE);
    const XrTargetMachineRepRecord *channel_rep =
        &channel.plan->machine_reps[channel.plan->slots[channel_slot].register_rep];
    REQUIRE(channel_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            channel_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL);
    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(channel.plan, channel_function, &frame) == XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(!frame);
    dispose_plan(&channel);

    require_rooted_object_execution_unavailable(&stub_mutex, "opaque_mutex", NULL, false);
    require_rooted_object_execution_unavailable(&stub_socket, "opaque_socket", NULL, false);
    require_rooted_object_execution_unavailable(&stub_foreign_handle, "opaque_foreign_handle", NULL, false);
    require_rooted_object_execution_unavailable(&stub_graph_node, "opaque_graph_node", NULL, false);
}

static void test_declared_class_and_native_storage_authority(void) {
    XrClassInfo class_info = {.name = "GraphNode", .xg_class_id = 812};
    XiClassData declaration = {
        .class_info = &class_info,
        .xg_class_id = class_info.xg_class_id,
        .class_name = class_info.name,
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    XrType declared = {
        .kind = XR_KIND_INSTANCE, .id = 12, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE,
        .instance = {.class_name = class_info.name, .class_ref = &class_info},
    };
    require_rooted_object_execution_unavailable(&declared, "declared_graph_owned", &declaration, false);
    require_rooted_object_execution_unavailable(&declared, "declared_graph_borrowed", &declaration, true);

    const XrStdlibNativeClassDefEntry *native = xr_stdlib_metadata_unique_native_class_span(
        "net", 3, "__NetConnStorage", strlen("__NetConnStorage"));
    REQUIRE(native && strcmp(native->builtin_kind, "XR_BK_NET_CONN_STORAGE") == 0 &&
            native->native_body_expr && native->native_body_expr[0] &&
            strcmp(native->source_wrapper, "NetConn") == 0 &&
            strcmp(native->source_storage_field, "_storage") == 0);
    XrType storage = {
        .kind = XR_KIND_INSTANCE, .id = 13, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE,
        .instance = {.class_name = native->name},
    };
    REQUIRE(xr_stdlib_metadata_resource_identity(native, &storage.instance.resource_id));
    /* Independent SHA-256 golden for the resource domain and length-prefixed
     * declaration (net, __NetConnStorage), truncated to the first 16 bytes. */
    static const XrStableId expected_resource = {{
        0x0b, 0x55, 0x1a, 0x7a, 0x09, 0x78, 0x02, 0x97,
        0x3f, 0x0b, 0x08, 0x7b, 0xd7, 0x96, 0x56, 0xc3,
    }};
    REQUIRE(xr_stable_id_equal(storage.instance.resource_id, expected_resource));
    XrStableId zero = {{0}};
    REQUIRE(!xr_stable_id_equal(storage.instance.resource_id, zero));
    require_rooted_object_execution_unavailable(&storage, "native_storage_owned", NULL, false);
    require_rooted_object_execution_unavailable(&storage, "native_storage_borrowed", NULL, true);

    /* A real source declaration may reuse a native spelling. Its nonzero
     * source identity prevents it from becoming registry-native storage. */
    class_info.name = native->name;
    declaration.class_name = native->name;
    declared.instance.class_name = native->name;
    require_rooted_object_execution_unavailable(&declared, "source_native_name_shadow", &declaration, false);
}

static void test_fabricated_geometry_and_capability_rejected(void) {
    PlanFixture fixture = build_plan(build_identity_semantic("opaque_tamper", &stub_graph_node, NULL, false));
    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
    uint32_t function = find_function_with_rep(fixture.plan, XR_MACHINE_REP_DYN_VALUE, &slot);
    REQUIRE(function != XR_SEMANTIC_INDEX_NONE && slot != XR_SEMANTIC_INDEX_NONE);
    XrTargetSlotRecord saved = fixture.plan->slots[slot];
    fixture.plan->slots[slot].size++;
    require_verify_rejected(fixture.plan, "XR_TARGET_1001");
    XrTypedFrame *frame = NULL;
    REQUIRE(create_frame(fixture.plan, function, &frame) == XR_TYPED_FRAME_SLOT_INVALID && !frame);
    fixture.plan->slots[slot] = saved;
    uint16_t provider = fixture.plan->capabilities[0].provider_role;
    fixture.plan->capabilities[0].provider_role = XR_TARGET_PROVIDER_ROLE_PANIC;
    require_verify_rejected(fixture.plan, "XR_TARGET_1004");
    fixture.plan->capabilities[0].provider_role = provider;
    xr_target_plan_compute_fingerprint(fixture.plan, &fixture.plan->fingerprint);
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic, sizeof(diagnostic)));
    require_execution_authority_unavailable(fixture.plan, function);
    dispose_plan(&fixture);
}

static void test_fabricated_adapters_fail_closed(void) {
    PlanFixture fixture = build_plan(build_raw_pointer_semantic());
    REQUIRE(fixture.plan->adapters_count == 0 && fixture.plan->adapters == NULL);
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
        xr_target_plan_compute_fingerprint(fixture.plan, &fixture.plan->fingerprint);
    }
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic, sizeof(diagnostic)));
    dispose_plan(&fixture);
}

typedef struct EntryResolver {
    const XiFunc *callee;
} EntryResolver;

static const XiFunc *resolve_entry(void *context, const XiFunc *current, const XiValue *call) {
    (void) current;
    EntryResolver *resolver = (EntryResolver *) context;
    return resolver && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "echo") == 0
               ? resolver->callee
               : NULL;
}

static int entry_suspendability(void *context, const XiFunc *current, const XiValue *call) {
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
    dependency_root->children = (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = echo;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    echo->parent_func = dependency_root;
    echo->nparams = echo->min_params = 1;
    echo->params = (XiValue **) xr_calloc(1, sizeof(*echo->params));
    REQUIRE(echo->params);
    echo->params[0] = xi_param(echo, echo_entry, 0, &stub_int);
    REQUIRE(echo->params[0]);
    XiValue *closure =
        xi_value_new(dependency_root, dependency_entry, XI_CLOSURE_NEW, &stub_scalar_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry, XI_SET_SHARED, &stub_unit, 1);
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
    XiModule *dependency_module =
        xi_module_new("test/opaque/provider.xr", "opaque_provider", dependency_root);
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
    attach_fixture_module(dependency_root, "opaque-provider-fixture");
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, diagnostic, sizeof(diagnostic)));
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency && xr_semantic_plan_source_export_count(dependency) == 1);

    XiFunc *caller_root = xi_func_new("opaque_consumer", &stub_unit);
    XiFunc *caller = xi_func_new("call_echo", &stub_int);
    REQUIRE(caller_root && caller);
    XiBlock *root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    caller_root->children = (XiFunc **) xr_calloc(1, sizeof(*caller_root->children));
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
    XiValue *namespace_ref =
        xi_value_new(caller_root, root_entry, XI_IMPORT_REF, &stub_module_namespace, 0);
    XiValue *namespace_alias =
        xi_value_new(caller_root, root_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *namespace_store = xi_value_new(caller_root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(namespace_ref && namespace_alias && namespace_store);
    namespace_ref->aux = &import;
    namespace_alias->args[0] = namespace_ref;
    namespace_alias->aux_int = XI_COPY_KIND_IDENTITY;
    namespace_store->args[0] = namespace_alias;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver =
        xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_module_namespace, 0);
    XiValue *receiver_alias =
        xi_value_new(caller, caller_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *argument = xi_const_int(caller, caller_entry, 41, &stub_int);
    XiValue *method = xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_int, 2);
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
    XiModule *caller_module =
        xi_module_new("test/opaque/consumer.xr", "opaque_consumer", caller_root);
    REQUIRE(caller_module);
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependencies[] = {dependency_module};
    attach_fixture_module(caller_root, "opaque-consumer-fixture");
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(caller_root, dependencies, 1, diagnostic,
                                                         sizeof(diagnostic)));
    XrSemanticPlan *semantic = xr_semantic_plan_retain(caller_root->semantic_plan);
    REQUIRE(semantic);

    PlanFixture fixture = {
        .semantic = semantic,
        .dependency = dependency,
        .profile = build_profile(),
    };
    const XrSemanticPlan *semantic_dependencies[] = {dependency};
    bool built = xr_target_plan_build_module_set(semantic, semantic_dependencies, 1, fixture.profile,
                                                &fixture.plan, diagnostic, sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "opaque entry target build failed: %s\n", diagnostic);
    REQUIRE(built);
    REQUIRE(fixture.plan && fixture.plan->entry_expectations_count == 1 &&
            fixture.plan->entry_expectations[0].adapter_kind == XR_TARGET_ENTRY_ADAPTER_IDENTITY);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
    return fixture;
}

static void test_non_identity_entry_rejected(void) {
    PlanFixture fixture = build_entry_plan();
    fixture.plan->entry_expectations[0].adapter_kind = XR_TARGET_ENTRY_ADAPTER_INVALID;
    require_verify_rejected(fixture.plan, "XR_TARGET_1005");
    fixture.plan->entry_expectations[0].adapter_kind = XR_TARGET_ENTRY_ADAPTER_IDENTITY;
    xr_target_plan_compute_fingerprint(fixture.plan, &fixture.plan->fingerprint);
    char diagnostic[512] = {0};
    REQUIRE(xr_target_plan_verify(fixture.plan, diagnostic, sizeof(diagnostic)));
    dispose_plan(&fixture);
}


static void test_native_authority_output_and_profile_boundary(void) {
    /* Keep the large caller snapshot off the stack so the default thread stack
     * exercises the runtime factories and lifecycle admission themselves. */
    XrRuntimeTargetAuthority *authority =
        (XrRuntimeTargetAuthority *) xr_malloc(sizeof(*authority));
    REQUIRE(authority != NULL);
    REQUIRE(xr_runtime_target_authority_native_hosted(NULL) ==
            XR_RUNTIME_ABI_INVALID_ARGUMENT);
    REQUIRE(xr_runtime_target_authority_native_freestanding(
                XR_TARGET_FOUNDATION_CAPABILITY_MASK, NULL) ==
            XR_RUNTIME_ABI_INVALID_ARGUMENT);
    static const uint64_t rejected_capabilities[] = {
        0,
        XR_TARGET_FOUNDATION_CAPABILITY_MASK |
            XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC_BOUNDARY),
    };
    for (size_t i = 0; i < sizeof(rejected_capabilities) /
                                sizeof(rejected_capabilities[0]); i++) {
        memset(authority, 0xa5, sizeof(*authority));
        REQUIRE(xr_runtime_target_authority_native_freestanding(
                    rejected_capabilities[i], authority) ==
                XR_RUNTIME_ABI_INVALID_ARGUMENT);
        const uint8_t *bytes = (const uint8_t *) authority;
        for (size_t byte = 0; byte < sizeof(*authority); byte++)
            REQUIRE(bytes[byte] == 0xa5);
    }
    REQUIRE(xr_runtime_target_authority_native_hosted(authority) == XR_RUNTIME_ABI_OK);
    REQUIRE(authority->machine.runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED &&
            authority->provider_count != 0);
    XrTargetProfileBuildInput input = {
        .machine = authority->machine,
        .runtime_abi = &authority->runtime_abi,
        .object_header_materialization = &authority->object_header_materialization,
        .string_contract = &authority->string_contract,
        .providers = authority->providers,
        .provider_count = authority->provider_count,
    };
    char diagnostic[512] = {0};
    XrTargetProfile *projected = NULL;
    XrTargetProfile *native = NULL;
    REQUIRE(xr_target_profile_build(&input, &projected, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_target_profile_build_native_hosted(&native, diagnostic,
                                                         sizeof(diagnostic)));
    REQUIRE(xr_target_profile_require_exact(projected, native, diagnostic,
                                            sizeof(diagnostic)));
    xr_target_profile_free(native);
    xr_target_profile_free(projected);
    REQUIRE(!xr_runtime_target_profile_build_native_hosted(NULL, diagnostic,
                                                          sizeof(diagnostic)));
    input.machine.data_layout.pointer.size++;
    projected = (XrTargetProfile *) (uintptr_t) 1;
    REQUIRE(!xr_target_profile_build(&input, &projected, diagnostic,
                                     sizeof(diagnostic)) &&
            projected == NULL);
    xr_free(authority);
}

int main(void) {
    test_native_authority_output_and_profile_boundary();
    test_raw_pointer_is_opaque_bytes();
    test_rooted_handles_are_not_frame_transport();
    test_declared_class_and_native_storage_authority();
    test_fabricated_geometry_and_capability_rejected();
    test_fabricated_adapters_fail_closed();
    test_non_identity_entry_rejected();
    puts("typed opaque boundary tests passed");
    return 0;
}
