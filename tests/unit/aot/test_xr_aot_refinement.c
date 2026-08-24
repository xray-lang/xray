/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_aot_refinement.c - TargetPlan-native AOT refinement KAT
 *
 * COVERAGE THIS FILE NO LONGER HAS
 *
 * Two verifiers used to sit beside the ones exercised here.
 * xr_aot_representation_refinement_verify re-derived every source value,
 * use-site, type and layout obligation from the live Xi graph, and
 * xr_aot_backend_run / xr_aot_representation_backend_run drove a record
 * visitor. Neither had a production caller, so both were removed along with
 * the assertions that only they could carry. Those assertions mutated a live
 * Xi graph after the plan was frozen and required the mutation to be
 * rejected:
 *
 *   - a string literal whose payload was swapped for another literal, and
 *     one whose type was made nullable      (XR_AOT_REFINEMENT_SOURCE_TYPE)
 *   - a closure whose callee was pointed at a detached copy of its child,
 *     and one pointed at the enclosing function       (... SOURCE_TYPE)
 *   - a store whose slot index was shifted by one (... SOURCE_IDENTITY)
 *   - a plan holding fewer adapters than the graph needs, and one holding an
 *     adapter the graph does not need     (... INCOMPLETE_COVERAGE)
 *   - a representation policy changed after the plan was frozen
 *                                              (... STALE_EVIDENCE)
 *   - a backend interface with a missing callback, a wrong ABI version, or a
 *     visit that fails midway                (... BACKEND_* issues, also gone)
 *
 * Nothing in the tree re-derives those facts from the live Xi graph today.
 * What remains is xr_aot_refinement_verify, which checks a frozen plan
 * against the TargetPlan baseline, and
 * xr_aot_representation_materialization_verify, which checks that a
 * backend-stage graph is the exact materialization of that plan; both run in
 * the driver and are exercised below.
 */

#include "../../../src/aot/refine/xr_aot_refinement.h"
#include "../../../src/aot/refine/xr_aot_tail_call_conformance.h"
#include "../../../src/aot/refine/xr_aot_representation_refinement.h"
#include "../../../src/aot/emit_c/xr_c_emission_plan.h"
#ifdef XAOT_BUNDLE_H
#error "refinement public API must not expose the legacy XaotBundle"
#endif
#ifdef XGLOBAL_SUMMARY_H
#error "refinement public API must not expose global analyzer evidence"
#endif
#ifdef XI_H
#error "refinement public API must not expose mutable compiler IR"
#endif
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/aot/xaot_bundle.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/aot/refine/xr_aot_scalar_value.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Complete the public opaque projection only for fail-closed mutation tests. */
struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    XrCCallArgumentEmissionView *call_arguments;
    uint32_t call_argument_count;
    XrCRecipeArgumentView *recipe_arguments;
    uint32_t recipe_argument_count;
    XrCCleanupEmissionView *cleanups;
    uint32_t cleanup_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static bool ensure_fixture_module_identity(XiFunc *root) {
    if (!root)
        return false;
    if (!root->module) {
        root->module = xi_module_new("fixture/aot_refinement.xr",
                                     root->name ? root->name : "aot_refinement_fixture", root);
        if (!root->module)
            return false;
    }
    if (!root->module->identity &&
        !xi_module_set_identity(root->module,
                                "memory-module-v1:id=25:aot-refinement-fixture-v1"))
        return false;
    return true;
}

static bool build_fixture_semantic_plan_and_attach(XiFunc *root, char *error,
                                                   size_t error_size) {
    if (!ensure_fixture_module_identity(root))
        return false;
    return xr_semantic_plan_build_and_attach(root, error, error_size);
}

typedef struct RefinementFixture {
    XrSemanticPlan *semantic_plan;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} RefinementFixture;

typedef struct RepresentationFixture {
    XiFunc *function;
    XiBlock *entry;
    XiValue *native_constant;
    XiValue *tagged_call;
    XiValue *rhs;
    XiValue *sum;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} RepresentationFixture;

typedef struct MaterializationFixture {
    XiFunc *function;
    XiBlock *entry;
    XiBlock *then_block;
    XiBlock *else_block;
    XiBlock *merge_block;
    XiValue *then_value;
    XiValue *else_value;
    XiPhi *phi;
    XiValue *sum;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} MaterializationFixture;

static XrTargetPlan *build_attached_target_plan(
    XiFunc *function, XrTargetProfile **out_profile);

typedef struct SharedScalarFixture {
    XiFunc *function;
    XiBlock *entry;
    XiValue *constant;
    XiValue *store;
    XiValue *load;
    XiValue *print;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} SharedScalarFixture;

typedef struct ClosureStorageFixture {
    XiFunc *function;
    XiFunc *child;
    XiBlock *entry;
    XiValue *closure;
    XiValue *store;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} ClosureStorageFixture;

typedef struct DirectLocalCalleeStorageFixture {
    XiFunc *function;
    XiFunc *caller;
    XiFunc *child;
    XiFunc *decoy;
    XiBlock *entry;
    XiValue *load;
    XiValue *call;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} DirectLocalCalleeStorageFixture;

typedef struct DirectLocalTaggedRefFixture {
    XiFunc *function;
    XiFunc *child;
    XiValue *parameter;
    XiValue *caller_storage;
    XiValue *place;
    XiValue *call;
    XiValue *writeback;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} DirectLocalTaggedRefFixture;

typedef struct DirectLocalGoCalleeStorageFixture {
    XiFunc *function;
    XiFunc *child;
    XiFunc *decoy;
    XiBlock *entry;
    XiValue *load;
    XiValue *go;
    XiValue *await;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} DirectLocalGoCalleeStorageFixture;

typedef struct SourceNamespaceStorageFixture {
    XiFunc *function;
    XiFunc *caller;
    XiModule *module;
    XiModule *dependency_module;
    XiImportRef import_ref;
    XiValue *namespace_ref;
    XiValue *namespace_alias;
    XiValue *namespace_retain;
    XiValue *namespace_store;
    XiValue *receiver;
    XiValue *receiver_alias;
    XiValue *call;
    XrSemanticPlan *dependency;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} SourceNamespaceStorageFixture;

typedef struct NativeNamespaceYieldableStorageFixture {
    XiFunc *function;
    XiFunc *caller;
    XiValue *namespace_ref;
    XiValue *namespace_load;
    XiValue *call;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} NativeNamespaceYieldableStorageFixture;

static XrType scalar_int = {
    .kind = XR_KIND_INT,
    .id = 1,
    .scalar_rep = XR_NATIVE_I64,
    .frozen = true,
};

static XrType scalar_bool = {
    .kind = XR_KIND_BOOL,
    .id = 2,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};

static XrType scalar_string = {
    .kind = XR_KIND_STRING,
    .id = 3,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};

static XrType scalar_unit = {
    .kind = XR_KIND_UNIT,
    .id = 4,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};

static XrType scalar_closure = {
    .kind = XR_KIND_FUNCTION,
    .id = 5,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .return_type = &scalar_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};

/* The production select fixture lowers its top-level callable token as an
 * opaque reference.  The direct-local SemanticPlan call target, not this
 * erased Xi type, is the callable authority. */
static XrType opaque_callable = {
    .kind = XR_KIND_UNKNOWN,
    .id = 6,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static XrType direct_local_go_closure = {
    .kind = XR_KIND_FUNCTION,
    .id = 7,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .return_type = &scalar_unit,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};

static XrType module_namespace = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 8,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static XrType scalar_byte = {
    .kind = XR_KIND_INT,
    .id = 9,
    .frozen = true,
    .scalar_rep = XR_NATIVE_U8,
};

static XrType borrowed_byte_slice = {
    .kind = XR_KIND_SLICE,
    .id = 10,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &scalar_byte},
};

static XrType scalar_u32 = {
    .kind = XR_KIND_INT,
    .id = 12,
    .frozen = true,
    .scalar_rep = XR_NATIVE_U32,
};

static XrType fixed_u32x4 = {
    .kind = XR_KIND_FIXED_ARRAY,
    .id = 13,
    .frozen = true,
    .is_value_type = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .fixed_array = {
        .element_type = &scalar_u32,
        .length = 4,
    },
};

static XrType direct_local_byte_array = {
    .kind = XR_KIND_ARRAY,
    .id = 14,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &scalar_byte},
};

static XrType direct_local_int_array = {
    .kind = XR_KIND_ARRAY,
    .id = 12,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &scalar_int},
};

static XrType tagged_string_array = {
    .kind = XR_KIND_ARRAY,
    .id = 18,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &scalar_string},
};

static XrFunctionParam array_hof_unary_int_params[] = {
    {.type = &scalar_int, .mode = XR_PARAM_READ},
};

static XrFunctionParam array_hof_reduce_params[] = {
    {.type = &scalar_int, .mode = XR_PARAM_READ},
    {.type = &scalar_int, .mode = XR_PARAM_READ},
};

static XrType array_hof_map_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 15,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = array_hof_unary_int_params,
        .param_count = 1,
        .min_params = 1,
        .return_type = &scalar_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};

static XrType array_hof_filter_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 16,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = array_hof_unary_int_params,
        .param_count = 1,
        .min_params = 1,
        .return_type = &scalar_bool,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};

static XrType array_hof_reduce_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 17,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = array_hof_reduce_params,
        .param_count = 2,
        .min_params = 2,
        .return_type = &scalar_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};

static XrType *task_unit_arguments[] = {&scalar_unit};

static XrType task_unit = {
    .kind = XR_KIND_INSTANCE,
    .id = 11,
    .frozen = true,
    .instance = {
        .class_name = "Task",
        .type_args = task_unit_arguments,
        .type_arg_count = 1,
    },
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("refinement_scalar_baseline", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &scalar_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic_plan = NULL;
    char error[512] = {0};
    REQUIRE(ensure_fixture_module_identity(function));
    REQUIRE(xr_semantic_plan_build(function, &semantic_plan, error, sizeof(error)));
    xi_func_free(function);
    return semantic_plan;
}

static XrTargetProfile *build_target_profile(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    return profile;
}

static RefinementFixture fixture_create(void) {
    RefinementFixture fixture = {0};
    fixture.semantic_plan = build_semantic_plan();
    fixture.target_profile = build_target_profile();
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic_plan, fixture.target_profile,
                                 &fixture.target_plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.target_plan));
    REQUIRE(xr_target_plan_completed_family_mask(fixture.target_plan) ==
            XR_TARGET_REQUIRED_FAMILIES);
    uint32_t call_count = UINT32_MAX;
    REQUIRE(xr_target_plan_calls(fixture.target_plan, &call_count) == NULL);
    REQUIRE(call_count == 0);
    return fixture;
}

static void fixture_free(RefinementFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xr_semantic_plan_free(fixture->semantic_plan);
    memset(fixture, 0, sizeof(*fixture));
}

static RepresentationFixture representation_fixture_create(void) {
    RepresentationFixture fixture = {0};
    fixture.function = xi_func_new("representation_refinement", &scalar_int);
    REQUIRE(fixture.function != NULL);
    fixture.entry = xi_block_new(fixture.function);
    REQUIRE(fixture.entry != NULL);
    fixture.native_constant =
        xi_const_int(fixture.function, fixture.entry, 40, &scalar_int);
    fixture.rhs = xi_value_new(fixture.function, fixture.entry, XI_UNBOX,
                               &scalar_int, 1);
    REQUIRE(fixture.native_constant != NULL && fixture.rhs != NULL);
    fixture.rhs->args[0] = fixture.native_constant;
    fixture.tagged_call = xi_value_new(fixture.function, fixture.entry,
                                       XI_BOX, &scalar_int, 1);
    REQUIRE(fixture.tagged_call != NULL);
    fixture.tagged_call->args[0] = fixture.rhs;
    fixture.sum = xi_value_new(fixture.function, fixture.entry, XI_ADD,
                               &scalar_int, 2);
    REQUIRE(fixture.sum != NULL);
    fixture.sum->args[0] = fixture.tagged_call;
    fixture.sum->args[1] = fixture.rhs;
    xi_block_set_return(fixture.entry, fixture.sum);
    fixture.function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    REQUIRE(xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile, &fixture.target_plan,
                                 error, sizeof(error)));
    return fixture;
}

static void representation_fixture_free(RepresentationFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static MaterializationFixture materialization_fixture_create(void) {
    MaterializationFixture fixture = {0};
    fixture.function = xi_func_new("representation_materialization",
                                   &scalar_int);
    REQUIRE(fixture.function != NULL);
    fixture.entry = xi_block_new(fixture.function);
    fixture.then_block = xi_block_new(fixture.function);
    fixture.else_block = xi_block_new(fixture.function);
    fixture.merge_block = xi_block_new(fixture.function);
    REQUIRE(fixture.entry && fixture.then_block && fixture.else_block &&
            fixture.merge_block);

    XiValue *condition = xi_param(fixture.function, fixture.entry, 0,
                                  &scalar_bool);
    REQUIRE(condition != NULL);
    fixture.function->nparams = 1;
    fixture.function->params =
        (XiValue **) xr_malloc(sizeof(*fixture.function->params));
    REQUIRE(fixture.function->params != NULL);
    fixture.function->params[0] = condition;
    fixture.then_value = xi_const_int(fixture.function, fixture.then_block,
                                      10, &scalar_int);
    fixture.else_value = xi_const_int(fixture.function, fixture.else_block,
                                      20, &scalar_int);
    REQUIRE(fixture.then_value && fixture.else_value);
    xi_block_set_if(fixture.entry, condition, fixture.then_block,
                    fixture.else_block);
    xi_block_set_jump(fixture.then_block, fixture.merge_block);
    xi_block_set_jump(fixture.else_block, fixture.merge_block);
    fixture.phi = xi_phi_new(fixture.function, fixture.merge_block,
                             &scalar_int, 2);
    REQUIRE(fixture.phi != NULL);
    fixture.phi->value.args[0] = fixture.then_value;
    fixture.phi->value.args[1] = fixture.else_value;
    XiValue *one = xi_const_int(fixture.function, fixture.merge_block, 1,
                                &scalar_int);
    fixture.sum = xi_value_new(fixture.function, fixture.merge_block, XI_ADD,
                               &scalar_int, 2);
    REQUIRE(one && fixture.sum);
    fixture.sum->args[0] = &fixture.phi->value;
    fixture.sum->args[1] = one;
    xi_block_set_return(fixture.merge_block, fixture.sum);
    fixture.function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    if (!xr_target_plan_build(fixture.function->semantic_plan,
                              fixture.target_profile,
                              &fixture.target_plan, error,
                              sizeof(error))) {
        fprintf(stderr, "materialization TargetPlan build failed: %s\n",
                error);
        abort();
    }
    return fixture;
}

static void materialization_fixture_free(MaterializationFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static SharedScalarFixture shared_scalar_fixture_create(void) {
    SharedScalarFixture fixture = {0};
    fixture.function = xi_func_new("shared_scalar_representation",
                                   &scalar_unit);
    REQUIRE(fixture.function != NULL);
    fixture.entry = xi_block_new(fixture.function);
    REQUIRE(fixture.entry != NULL);
    fixture.constant = xi_const_int(fixture.function, fixture.entry, 42,
                                    &scalar_int);
    fixture.store = xi_value_new(fixture.function, fixture.entry,
                                 XI_SET_SHARED, &scalar_unit, 1);
    fixture.load = xi_value_new(fixture.function, fixture.entry,
                                XI_GET_SHARED, &scalar_int, 0);
    fixture.print = xi_value_new(fixture.function, fixture.entry, XI_PRINT,
                                 &scalar_unit, 1);
    REQUIRE(fixture.constant && fixture.store && fixture.load && fixture.print);
    fixture.store->aux_int = 0;
    fixture.store->args[0] = fixture.constant;
    fixture.load->aux_int = 0;
    fixture.print->args[0] = fixture.load;
    xi_block_set_return(fixture.entry, NULL);
    fixture.function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    REQUIRE(xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile,
                                 &fixture.target_plan, error,
                                 sizeof(error)));
    return fixture;
}

static void shared_scalar_fixture_free(SharedScalarFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static ClosureStorageFixture closure_storage_fixture_create(bool captured) {
    ClosureStorageFixture fixture = {0};
    fixture.function = xi_func_new(captured ? "capturing_closure_storage"
                                            : "exact_closure_storage",
                                   &scalar_unit);
    fixture.child = xi_func_new(captured ? "capturing_closure_child"
                                         : "exact_closure_child",
                                &scalar_int);
    REQUIRE(fixture.function != NULL && fixture.child != NULL);
    fixture.entry = xi_block_new(fixture.function);
    XiBlock *child_entry = xi_block_new(fixture.child);
    REQUIRE(fixture.entry != NULL && child_entry != NULL);
    XiValue *child_result =
        xi_const_int(fixture.child, child_entry, 42, &scalar_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);

    fixture.function->children =
        (XiFunc **) xr_calloc(1, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children != NULL);
    fixture.function->children[0] = fixture.child;
    fixture.function->nchildren = fixture.function->children_cap = 1;
    fixture.child->parent_func = fixture.function;

    XiValue *captured_value = NULL;
    if (captured) {
        captured_value =
            xi_const_int(fixture.function, fixture.entry, 7, &scalar_int);
        REQUIRE(captured_value != NULL);
        fixture.child->ncaptures = 1;
        fixture.child->captures[0] = (XiCapture) {
            .source = XI_CAPTURE_SRC_REG,
            .capture_kind = XI_CAPTURE_BY_COPY,
            .type = &scalar_int,
            .value = captured_value,
            .name = "captured",
            .storage_domain = XR_STORAGE_EXEC_LOCAL,
            .value_capability = XR_SEM_VALUE_CONST,
        };
    }
    fixture.closure = xi_value_new(fixture.function, fixture.entry,
                                   XI_CLOSURE_NEW, &scalar_closure,
                                   captured ? 1 : 0);
    REQUIRE(fixture.closure != NULL);
    fixture.closure->aux = fixture.child;
    if (captured)
        fixture.closure->args[0] = captured_value;
    fixture.store = xi_value_new(fixture.function, fixture.entry,
                                 XI_SET_SHARED, &scalar_unit, 1);
    REQUIRE(fixture.store != NULL);
    fixture.store->args[0] = fixture.closure;
    fixture.store->aux_int = 0;
    xi_block_set_return(fixture.entry, NULL);
    fixture.function->stage = fixture.child->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    REQUIRE(xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile,
                                 &fixture.target_plan, error,
                                 sizeof(error)));
    return fixture;
}

static void closure_storage_fixture_free(ClosureStorageFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static DirectLocalCalleeStorageFixture
direct_local_callee_storage_fixture_create(bool extra_use, bool tail_call) {
    DirectLocalCalleeStorageFixture fixture = {0};
    fixture.function =
        xi_func_new("direct_local_shared_root", &scalar_int);
    fixture.caller =
        xi_func_new("direct_local_shared_caller", &scalar_int);
    fixture.child = xi_func_new("direct_local_shared_target", &scalar_int);
    fixture.decoy = xi_func_new("direct_local_shared_decoy", &scalar_int);
    REQUIRE(fixture.function && fixture.caller && fixture.child && fixture.decoy);
    fixture.entry = xi_block_new(fixture.function);
    XiBlock *caller_entry = xi_block_new(fixture.caller);
    XiBlock *child_entry = xi_block_new(fixture.child);
    XiBlock *decoy_entry = xi_block_new(fixture.decoy);
    REQUIRE(fixture.entry && caller_entry && child_entry && decoy_entry);
    XiValue *child_result =
        xi_const_int(fixture.child, child_entry, 42, &scalar_int);
    XiValue *decoy_result =
        xi_const_int(fixture.decoy, decoy_entry, 7, &scalar_int);
    REQUIRE(child_result && decoy_result);
    xi_block_set_return(child_entry, child_result);
    xi_block_set_return(decoy_entry, decoy_result);

    fixture.function->children =
        (XiFunc **) xr_calloc(3, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children != NULL);
    fixture.function->children[0] = fixture.caller;
    fixture.function->children[1] = fixture.child;
    fixture.function->children[2] = fixture.decoy;
    fixture.function->nchildren = fixture.function->children_cap = 3;
    fixture.caller->parent_func = fixture.function;
    fixture.child->parent_func = fixture.function;
    fixture.decoy->parent_func = fixture.function;

    XiValue *closure = xi_value_new(fixture.function, fixture.entry,
                                    XI_CLOSURE_NEW, &scalar_closure, 0);
    XiValue *store = xi_value_new(fixture.function, fixture.entry,
                                  XI_SET_SHARED, &scalar_unit, 1);
    fixture.load = xi_value_new(fixture.caller, caller_entry,
                                XI_GET_SHARED, &opaque_callable, 0);
    fixture.call = xi_value_new(fixture.caller, caller_entry,
                                tail_call ? XI_TAIL_CALL : XI_CALL,
                                &scalar_int, 1);
    REQUIRE(closure && store && fixture.load && fixture.call);
    closure->aux = fixture.child;
    store->aux_int = 0;
    store->args[0] = closure;
    fixture.load->aux_int = 0;
    fixture.call->args[0] = fixture.load;
    if (extra_use) {
        XiValue *unexpected = xi_value_new(fixture.caller, caller_entry,
                                           XI_PRINT, &scalar_unit, 1);
        REQUIRE(unexpected != NULL);
        unexpected->args[0] = fixture.load;
    }
    xi_block_set_return(fixture.entry, NULL);
    xi_block_set_return(caller_entry, fixture.call);
    fixture.function->nshared = 1;
    fixture.function->shared_slot_funcs = (XiFunc **) xi_func_arena_alloc(
        fixture.function, sizeof(*fixture.function->shared_slot_funcs));
    REQUIRE(fixture.function->shared_slot_funcs != NULL);
    fixture.function->shared_slot_funcs[0] = fixture.child;
    fixture.function->shared_slot_func_count = 1;
    fixture.function->stage = fixture.caller->stage = fixture.child->stage =
        fixture.decoy->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    bool built = xr_target_plan_build(
        fixture.function->semantic_plan, fixture.target_profile,
        &fixture.target_plan, error, sizeof(error));
    if (extra_use) {
        REQUIRE(!built && fixture.target_plan == NULL);
    } else {
        if (!built)
            fprintf(stderr, "direct-local TargetPlan build failed: %s\n",
                    error);
        REQUIRE(built && fixture.target_plan != NULL);
    }
    return fixture;
}

static void direct_local_callee_storage_fixture_free(
    DirectLocalCalleeStorageFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static DirectLocalTaggedRefFixture direct_local_tagged_ref_fixture_create(void) {
    DirectLocalTaggedRefFixture fixture = {0};
    fixture.function =
        xi_func_new("direct_local_tagged_ref_root", &scalar_int);
    fixture.child =
        xi_func_new("direct_local_tagged_ref_target", &scalar_int);
    REQUIRE(fixture.function && fixture.child);
    XiBlock *root_entry = xi_block_new(fixture.function);
    XiBlock *child_entry = xi_block_new(fixture.child);
    REQUIRE(root_entry && child_entry);

    fixture.child->nparams = fixture.child->min_params = 1;
    fixture.child->params = (XiValue **) xr_calloc(
        1, sizeof(*fixture.child->params));
    REQUIRE(fixture.child->params != NULL);
    fixture.parameter =
        xi_param(fixture.child, child_entry, 0, &direct_local_byte_array);
    REQUIRE(fixture.parameter != NULL);
    fixture.child->params[0] = fixture.parameter;
    REQUIRE(xi_func_set_param_passing_mode(fixture.child, 0, XR_PARAM_REF));
    fixture.parameter->transfer_mode = XR_TRANSFER_SHARE;
    fixture.child->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
        fixture.child, (uint32_t) sizeof(*fixture.child->arc_borrow_sig));
    REQUIRE(fixture.child->arc_borrow_sig != NULL);
    fixture.child->arc_borrow_sig->nparams = 1;
    fixture.child->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    fixture.child->arc_borrow_sig->valid = true;
    XiValue *loaded = xi_value_new(fixture.child, child_entry, XI_PLACE_LOAD,
                                   &direct_local_byte_array, 1);
    XiValue *index = xi_const_int(fixture.child, child_entry, 0,
                                  &scalar_int);
    XiValue *element = xi_const_int(fixture.child, child_entry, 7,
                                    &scalar_byte);
    XiValue *write = xi_value_new(fixture.child, child_entry, XI_INDEX_SET,
                                  &scalar_unit, 3);
    XiValue *length = xi_value_new(fixture.child, child_entry, XI_LEN,
                                   &scalar_int, 1);
    REQUIRE(loaded && index && element && write && length);
    loaded->args[0] = fixture.parameter;
    write->args[0] = fixture.parameter;
    write->args[1] = index;
    write->args[2] = element;
    length->args[0] = loaded;
    xi_block_set_return(child_entry, length);

    fixture.function->children =
        (XiFunc **) xr_calloc(1, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children != NULL);
    fixture.function->children[0] = fixture.child;
    fixture.function->nchildren = fixture.function->children_cap = 1;
    fixture.child->parent_func = fixture.function;

    XiValue *closure = xi_value_new(fixture.function, root_entry,
                                    XI_CLOSURE_NEW, &scalar_closure, 0);
    XiValue *closure_store = xi_value_new(
        fixture.function, root_entry, XI_SET_SHARED, &scalar_unit, 1);
    XiValue *callable = xi_value_new(fixture.function, root_entry,
                                     XI_GET_SHARED, &opaque_callable, 0);
    fixture.caller_storage = xi_value_new(
        fixture.function, root_entry, XI_GET_SHARED,
        &direct_local_byte_array, 0);
    fixture.place = xi_value_new(fixture.function, root_entry, XI_LOCAL_ADDR,
                                 &direct_local_byte_array, 1);
    REQUIRE(closure && closure_store && callable && fixture.caller_storage &&
            fixture.place);
    closure->aux = fixture.child;
    closure_store->args[0] = closure;
    closure_store->aux_int = 0;
    callable->aux_int = 0;
    fixture.caller_storage->aux_int = 1;
    fixture.place->args[0] = fixture.caller_storage;

    XiCallPlan *call_plan = (XiCallPlan *) xi_func_arena_alloc(
        fixture.function, (uint32_t) sizeof(*call_plan));
    XiCallArgPlan *argument_plan = (XiCallArgPlan *) xi_func_arena_alloc(
        fixture.function, (uint32_t) sizeof(*argument_plan));
    REQUIRE(call_plan && argument_plan);
    memset(call_plan, 0, sizeof(*call_plan));
    memset(argument_plan, 0, sizeof(*argument_plan));
    argument_plan->param_mode = XR_PARAM_REF;
    argument_plan->access = XR_CALL_ARG_REF;
    argument_plan->origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    argument_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    argument_plan->escape = XI_PLACE_ESCAPE_NONE;
    argument_plan->addressable = true;
    argument_plan->origin_var_id = 0;
    argument_plan->place = fixture.caller_storage;
    call_plan->args = argument_plan;
    call_plan->nargs = 1;
    call_plan->verified = true;

    fixture.call = xi_value_new(fixture.function, root_entry, XI_CALL,
                                &scalar_int, 2);
    REQUIRE(fixture.call != NULL);
    fixture.call->args[0] = callable;
    fixture.call->args[1] = fixture.place;
    fixture.call->call_plan = call_plan;
    fixture.writeback = xi_value_new(fixture.function, root_entry,
                                     XI_PLACE_LOAD,
                                     &direct_local_byte_array, 1);
    XiValue *retain = xi_value_new(fixture.function, root_entry, XI_RETAIN,
                                   &direct_local_byte_array, 1);
    XiValue *store = xi_value_new(fixture.function, root_entry, XI_SET_SHARED,
                                  &scalar_unit, 1);
    REQUIRE(fixture.writeback && retain && store);
    fixture.writeback->args[0] = fixture.place;
    retain->args[0] = fixture.writeback;
    store->args[0] = fixture.writeback;
    store->aux_int = 1;
    xi_block_set_return(root_entry, fixture.call);
    fixture.function->nshared = 2;
    fixture.function->shared_slot_funcs =
        (XiFunc **) xi_func_arena_alloc(
            fixture.function,
            (uint32_t) sizeof(*fixture.function->shared_slot_funcs));
    REQUIRE(fixture.function->shared_slot_funcs != NULL);
    fixture.function->shared_slot_funcs[0] = fixture.child;
    fixture.function->shared_slot_func_count = 1;
    fixture.function->stage = fixture.child->stage = XI_STAGE_OPTIMIZED;
    XiModule *module = xi_module_new("fixtures/direct_local_tagged_ref.xr",
                                    "direct_local_tagged_ref", fixture.function);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(
        module, "memory-module-v1:id=34:direct-local-tagged-ref-fixture-v1"));
    fixture.function->module = module;

    char error[512] = {0};
    bool built = build_fixture_semantic_plan_and_attach(
        fixture.function, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local tagged ref SemanticPlan failed: %s\n",
                error);
    REQUIRE(built && fixture.function->semantic_plan != NULL);
    fixture.target_profile = build_target_profile();
    built = xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile,
                                 &fixture.target_plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local tagged ref TargetPlan failed: %s\n",
                error);
    REQUIRE(built && fixture.target_plan != NULL);
    return fixture;
}

static void direct_local_tagged_ref_fixture_free(
    DirectLocalTaggedRefFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static int source_namespace_suspendability(void *ud, const XiFunc *current,
                                           const XiValue *call) {
    (void) ud;
    (void) current;
    return call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "worker") == 0
               ? 1
               : -1;
}

static const XiFunc *source_namespace_resolve_method(
    void *ud, const XiFunc *current, const XiValue *call) {
    (void) current;
    return ud && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "worker") == 0
               ? (const XiFunc *) ud
               : NULL;
}

static SourceNamespaceStorageFixture source_namespace_storage_fixture_create(
    bool extra_use, bool standalone) {
    SourceNamespaceStorageFixture fixture = {0};
    XiFunc *dependency_root =
        xi_func_new("source_namespace_dependency_init", &scalar_unit);
    XiFunc *worker = xi_func_new("worker", &scalar_unit);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *worker_entry = xi_block_new(worker);
    REQUIRE(dependency_root && worker && dependency_entry && worker_entry);
    dependency_entry->sealed = worker_entry->sealed = true;
    dependency_root->children =
        (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = worker;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    worker->parent_func = dependency_root;
    XiValue *closure = xi_value_new(dependency_root, dependency_entry,
                                    XI_CLOSURE_NEW, &scalar_closure, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry,
                                  XI_SET_SHARED, &scalar_unit, 1);
    REQUIRE(closure && store);
    closure->aux = worker;
    store->args[0] = closure;
    store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    XiValue *yield = xi_value_new(worker, worker_entry, XI_YIELD,
                                  &scalar_unit, 0);
    REQUIRE(yield);
    xi_block_set_return(worker_entry, NULL);
    dependency_root->stage = worker->stage = XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = worker->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = worker->stage = XI_STAGE_OPTIMIZED;
    fixture.dependency_module = xi_module_new(
        "fixture/source_namespace_dependency.xr", "source_namespace_dependency",
        dependency_root);
    REQUIRE(fixture.dependency_module);
    REQUIRE(xi_module_set_identity(
        fixture.dependency_module,
        "memory-module-v1:id=30:source-namespace-dependency-v1"));
    dependency_root->module = fixture.dependency_module;
    fixture.dependency_module->nslots = 1;
    fixture.dependency_module->nexports = 1;
    fixture.dependency_module->exports = (XiModuleExport *) xr_calloc(
        1, sizeof(*fixture.dependency_module->exports));
    REQUIRE(fixture.dependency_module->exports);
    fixture.dependency_module->exports[0].name = "worker";
    fixture.dependency_module->exports[0].shared_slot = 0;
    fixture.dependency_module->exports[0].function = worker;

    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(dependency_root, error,
                                              sizeof(error)));
    fixture.dependency =
        xr_semantic_plan_retain(dependency_root->semantic_plan);

    fixture.function =
        xi_func_new("source_namespace_caller_init", &scalar_unit);
    fixture.caller = xi_func_new("source_namespace_caller", &scalar_unit);
    XiBlock *root_entry = xi_block_new(fixture.function);
    XiBlock *caller_entry = xi_block_new(fixture.caller);
    REQUIRE(fixture.function && fixture.caller && root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    fixture.function->children =
        (XiFunc **) xr_calloc(1, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children);
    fixture.function->children[0] = fixture.caller;
    fixture.function->nchildren = fixture.function->children_cap = 1;
    fixture.caller->parent_func = fixture.function;
    fixture.import_ref = (XiImportRef) {
        .module_path = "fixture/source_namespace_dependency.xr",
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = fixture.dependency_module,
    };
    fixture.namespace_ref = xi_value_new(fixture.function, root_entry,
                                          XI_IMPORT_REF,
                                          &module_namespace, 0);
    fixture.namespace_alias = standalone
                                  ? NULL
                                  : xi_value_new(fixture.function, root_entry,
                                                 XI_COPY,
                                                 &module_namespace, 1);
    fixture.namespace_retain = xi_value_new(fixture.function, root_entry,
                                             XI_RETAIN,
                                             &module_namespace, 1);
    fixture.namespace_store = xi_value_new(fixture.function, root_entry,
                                             XI_SET_SHARED,
                                             &scalar_unit, 1);
    REQUIRE(fixture.namespace_ref && fixture.namespace_retain &&
            fixture.namespace_store &&
            (standalone || fixture.namespace_alias));
    XiImportRef *live_import = (XiImportRef *) xi_func_arena_alloc(
        fixture.function, sizeof(*live_import));
    REQUIRE(live_import);
    *live_import = fixture.import_ref;
    fixture.namespace_ref->aux = live_import;
    if (fixture.namespace_alias) {
        fixture.namespace_alias->args[0] = fixture.namespace_ref;
        fixture.namespace_alias->aux_int = XI_COPY_KIND_IDENTITY;
    }
    fixture.namespace_retain->args[0] = fixture.namespace_ref;
    fixture.namespace_store->args[0] = fixture.namespace_alias
                                           ? fixture.namespace_alias
                                           : fixture.namespace_ref;
    fixture.namespace_store->aux_int = 0;
    if (extra_use) {
        XiValue *unexpected = xi_value_new(fixture.function, root_entry,
                                           XI_PRINT, &scalar_unit, 1);
        REQUIRE(unexpected);
        unexpected->args[0] = fixture.namespace_ref;
    }
    fixture.function->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    if (!standalone) {
        fixture.receiver = xi_value_new(fixture.caller, caller_entry,
                                         XI_GET_SHARED, &module_namespace, 0);
        fixture.receiver_alias = xi_value_new(fixture.caller, caller_entry,
                                               XI_COPY, &module_namespace, 1);
        fixture.call = xi_value_new(fixture.caller, caller_entry,
                                    XI_CALL_METHOD, &scalar_unit, 1);
        REQUIRE(fixture.receiver && fixture.receiver_alias && fixture.call);
        fixture.receiver->aux_int = 0;
        fixture.receiver_alias->args[0] = fixture.receiver;
        fixture.receiver_alias->aux_int = XI_COPY_KIND_IDENTITY;
        fixture.call->args[0] = fixture.receiver_alias;
        fixture.call->aux = (void *) "worker";
        fixture.call->aux_int = 0;
    }
    xi_block_set_return(caller_entry, NULL);
    fixture.function->stage = fixture.caller->stage =
        XI_STAGE_SEMANTIC_LOWERED;
    fixture.function->invariant_mask = fixture.caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    XiCoroResolver resolver = {
        .resolve_method = source_namespace_resolve_method,
        .call_suspendability = source_namespace_suspendability,
        .ud = worker,
    };
    REQUIRE(xi_coro_lower(fixture.function, &resolver));
    fixture.function->stage = fixture.caller->stage = XI_STAGE_OPTIMIZED;
    fixture.module = xi_module_new("fixture/source_namespace_caller.xr",
                                   "source_namespace_caller",
                                   fixture.function);
    REQUIRE(fixture.module);
    REQUIRE(xi_module_set_identity(
        fixture.module,
        "memory-module-v1:id=26:source-namespace-caller-v1"));
    fixture.function->module = fixture.module;
    fixture.module->nslots = 1;
    XiModule *dependencies[] = {fixture.dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(
        fixture.function, dependencies, 1, error, sizeof(error)));

    fixture.target_profile = build_target_profile();
    const XrSemanticPlan *semantic_dependencies[] = {fixture.dependency};
    bool built = xr_target_plan_build_module_set(
        fixture.function->semantic_plan, semantic_dependencies, 1,
        fixture.target_profile, &fixture.target_plan, error, sizeof(error));
    if (extra_use) {
        REQUIRE(!built && fixture.target_plan == NULL);
    } else {
        if (!built) {
            char semantic_error[512] = {0};
            bool semantic_verified = xr_semantic_plan_verify_module_set(
                fixture.function->semantic_plan, semantic_dependencies, 1,
                semantic_error, sizeof(semantic_error));
            fprintf(stderr, "source namespace TargetPlan build failed: %s\n",
                    error);
            fprintf(stderr, "source namespace semantic verify=%d: %s\n",
                    semantic_verified, semantic_error);
        }
        REQUIRE(built && fixture.target_plan);
    }
    return fixture;
}

static void source_namespace_storage_fixture_free(
    SourceNamespaceStorageFixture *fixture) {
    XiFunc *dependency_function = fixture->dependency_module
                                      ? fixture->dependency_module->init
                                      : NULL;
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    xi_func_free(dependency_function);
    xr_semantic_plan_free(fixture->dependency);
    memset(fixture, 0, sizeof(*fixture));
}

static int native_namespace_suspendability(void *ud, const XiFunc *current,
                                           const XiValue *call) {
    (void) ud;
    (void) current;
    return call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "sleep") == 0
               ? 1
               : -1;
}

static NativeNamespaceYieldableStorageFixture
native_namespace_yieldable_storage_fixture_create(bool extra_use) {
    NativeNamespaceYieldableStorageFixture fixture = {0};
    fixture.function = xi_func_new("native_namespace_root", &scalar_unit);
    fixture.caller = xi_func_new("native_namespace_caller", &scalar_unit);
    XiBlock *root_entry = xi_block_new(fixture.function);
    XiBlock *caller_entry = xi_block_new(fixture.caller);
    REQUIRE(fixture.function && fixture.caller && root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    fixture.function->children =
        (XiFunc **) xr_calloc(1, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children != NULL);
    fixture.function->children[0] = fixture.caller;
    fixture.function->nchildren = fixture.function->children_cap = 1;
    fixture.caller->parent_func = fixture.function;

    XiImportRef *import_ref = (XiImportRef *) xi_func_arena_alloc(
        fixture.function, sizeof(*import_ref));
    REQUIRE(import_ref != NULL);
    *import_ref = (XiImportRef) {
        .module_path = "time",
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolution_attempted = true,
    };
    fixture.namespace_ref = xi_value_new(
        fixture.function, root_entry, XI_IMPORT_REF, &module_namespace, 0);
    XiValue *retain = xi_value_new(fixture.function, root_entry, XI_RETAIN,
                                   &module_namespace, 1);
    XiValue *store = xi_value_new(fixture.function, root_entry, XI_SET_SHARED,
                                  &scalar_unit, 1);
    REQUIRE(fixture.namespace_ref && retain && store);
    fixture.namespace_ref->aux = import_ref;
    retain->args[0] = fixture.namespace_ref;
    store->args[0] = fixture.namespace_ref;
    store->aux_int = 0;
    fixture.function->nshared = 1;
    xi_block_set_return(root_entry, NULL);

    fixture.namespace_load = xi_value_new(
        fixture.caller, caller_entry, XI_GET_SHARED, &module_namespace, 0);
    XiValue *argument =
        xi_const_int(fixture.caller, caller_entry, 1, &scalar_int);
    fixture.call = xi_value_new(fixture.caller, caller_entry, XI_CALL_METHOD,
                                &scalar_unit, 2);
    REQUIRE(fixture.namespace_load && argument && fixture.call);
    fixture.namespace_load->aux_int = 0;
    fixture.call->args[0] = fixture.namespace_load;
    fixture.call->args[1] = argument;
    fixture.call->aux = (void *) "sleep";
    fixture.call->aux_int = 0;
    if (extra_use) {
        XiValue *unexpected = xi_value_new(fixture.caller, caller_entry,
                                           XI_PRINT, &scalar_unit, 1);
        REQUIRE(unexpected != NULL);
        unexpected->args[0] = fixture.namespace_load;
    }
    xi_block_set_return(caller_entry, NULL);
    fixture.function->stage = fixture.caller->stage =
        XI_STAGE_SEMANTIC_LOWERED;
    fixture.function->invariant_mask = fixture.caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    XiCoroResolver resolver = {
        .call_suspendability = native_namespace_suspendability,
    };
    REQUIRE(xi_coro_lower(fixture.function, &resolver));
    fixture.function->stage = fixture.caller->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    bool built = build_fixture_semantic_plan_and_attach(
        fixture.function, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native namespace SemanticPlan build failed: %s\n",
                error);
    REQUIRE(built);
    fixture.target_profile = build_target_profile();
    built = xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile,
                                 &fixture.target_plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native namespace TargetPlan build failed: %s\n",
                error);
    REQUIRE(built && fixture.target_plan);
    return fixture;
}

static void native_namespace_yieldable_storage_fixture_free(
    NativeNamespaceYieldableStorageFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static DirectLocalGoCalleeStorageFixture
direct_local_go_callee_storage_fixture_create(bool extra_use,
                                               bool second_store) {
    DirectLocalGoCalleeStorageFixture fixture = {0};
    fixture.function = xi_func_new("direct_local_shared_go", &scalar_unit);
    fixture.child = xi_func_new("direct_local_go_target", &scalar_unit);
    fixture.decoy = xi_func_new("direct_local_go_decoy", &scalar_unit);
    REQUIRE(fixture.function && fixture.child && fixture.decoy);
    fixture.entry = xi_block_new(fixture.function);
    XiBlock *child_entry = xi_block_new(fixture.child);
    XiBlock *decoy_entry = xi_block_new(fixture.decoy);
    REQUIRE(fixture.entry && child_entry && decoy_entry);
    xi_block_set_return(child_entry, NULL);
    xi_block_set_return(decoy_entry, NULL);
    fixture.function->children =
        (XiFunc **) xr_calloc(2, sizeof(*fixture.function->children));
    REQUIRE(fixture.function->children != NULL);
    fixture.function->children[0] = fixture.child;
    fixture.function->children[1] = fixture.decoy;
    fixture.function->nchildren = fixture.function->children_cap = 2;
    fixture.child->parent_func = fixture.function;
    fixture.decoy->parent_func = fixture.function;
    XiValue *closure = xi_value_new(fixture.function, fixture.entry,
                                    XI_CLOSURE_NEW,
                                    &direct_local_go_closure, 0);
    XiValue *store = xi_value_new(fixture.function, fixture.entry,
                                  XI_SET_SHARED, &scalar_unit, 1);
    fixture.load = xi_value_new(fixture.function, fixture.entry,
                                XI_GET_SHARED, &opaque_callable, 0);
    fixture.go = xi_value_new(fixture.function, fixture.entry,
                              XI_GO, &task_unit, 1);
    fixture.await = xi_value_new(fixture.function, fixture.entry,
                                 XI_AWAIT, &scalar_unit, 1);
    REQUIRE(closure && store && fixture.load && fixture.go && fixture.await);
    closure->aux = fixture.child;
    store->aux_int = 0;
    store->args[0] = closure;
    if (second_store) {
        XiValue *decoy_closure = xi_value_new(
            fixture.function, fixture.entry, XI_CLOSURE_NEW,
            &direct_local_go_closure, 0);
        XiValue *duplicate = xi_value_new(
            fixture.function, fixture.entry, XI_SET_SHARED,
            &scalar_unit, 1);
        REQUIRE(decoy_closure && duplicate);
        decoy_closure->aux = fixture.decoy;
        duplicate->aux_int = 0;
        duplicate->args[0] = decoy_closure;
    }
    fixture.load->aux_int = 0;
    fixture.go->args[0] = fixture.load;
    fixture.await->args[0] = fixture.go;
    fixture.await->aux_int = XI_AWAIT_AUX_CONSUME_TASK;
    if (extra_use) {
        XiValue *unexpected = xi_value_new(
            fixture.function, fixture.entry, XI_PRINT, &scalar_unit, 1);
        REQUIRE(unexpected != NULL);
        unexpected->args[0] = fixture.load;
    }
    xi_block_set_return(fixture.entry, NULL);
    fixture.function->nshared = 1;
    fixture.function->shared_slot_funcs = (XiFunc **) xi_func_arena_alloc(
        fixture.function, sizeof(*fixture.function->shared_slot_funcs));
    REQUIRE(fixture.function->shared_slot_funcs != NULL);
    fixture.function->shared_slot_funcs[0] = fixture.child;
    fixture.function->shared_slot_func_count = 1;
    fixture.entry->sealed = child_entry->sealed = decoy_entry->sealed = true;
    fixture.function->stage = fixture.child->stage =
        fixture.decoy->stage = XI_STAGE_SEMANTIC_LOWERED;
    fixture.function->invariant_mask = fixture.child->invariant_mask =
        fixture.decoy->invariant_mask =
            xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(fixture.function, NULL));
    fixture.function->stage = fixture.child->stage =
        fixture.decoy->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    bool semantic_built = build_fixture_semantic_plan_and_attach(
        fixture.function, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "go fixture SemanticPlan failed: %s\n", error);
    REQUIRE(semantic_built);
    fixture.target_profile = build_target_profile();
    bool built = xr_target_plan_build(
        fixture.function->semantic_plan, fixture.target_profile,
        &fixture.target_plan, error, sizeof(error));
    if (extra_use || second_store)
        REQUIRE(!built && fixture.target_plan == NULL);
    else {
        if (!built)
            fprintf(stderr, "go fixture TargetPlan failed: %s\n", error);
        REQUIRE(built && fixture.target_plan != NULL);
    }
    return fixture;
}

static void direct_local_go_callee_storage_fixture_free(
    DirectLocalGoCalleeStorageFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fingerprint_is_zero_bytes(const XrFingerprint *fingerprint) {
    static const XrFingerprint zero = {{0}};
    return xr_fingerprint_equal(*fingerprint, zero);
}

static bool test_pair_identity(const char *domain, XrStableId first, XrStableId second,
                               uint32_t ordinal, XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u", domain,
                           first_hex, second_hex, ordinal);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

/* An exported namespace callee is a real call row that the direct-call
 * validator must decline: the callee lives behind a dependency export, so this
 * plan cannot prove it is the one that will be called. */
static XrAotRefinementPlan *build_refused_plan(
    const XrTargetPlan *target_plan, XrAotRefinementDiagnostic *diag) {
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(target_plan, diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol = xr_aot_refinement_direct_call_protocol(27901);
    XrAotDirectCallRequest request = {.target_call_index = 0};
    uint32_t decision = 0;
    REQUIRE(xr_aot_refinement_try_direct_call(
        builder, &protocol, target_plan, &request, &decision, diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(diag->issue == XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED);
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(builder, target_plan, &plan, diag));
    xr_aot_refinement_builder_free(builder);
    return plan;
}

static void test_open_target_direct_call_refuses_without_baseline_change(void) {
    SourceNamespaceStorageFixture fixture =
        source_namespace_storage_fixture_create(false, false);
    XrFingerprint semantic_before =
        xr_target_plan_semantic_fingerprint(fixture.target_plan);
    XrFingerprint target_before =
        xr_target_plan_fingerprint(fixture.target_plan);
    XrFingerprint profile_before =
        xr_target_profile_fingerprint(fixture.target_profile);
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_refused_plan(fixture.target_plan, &diag);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified);
    REQUIRE(view.record_count == 1);
    REQUIRE(view.baseline.completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(xr_fingerprint_equal(view.baseline.semantic_fingerprint,
                                 semantic_before));
    REQUIRE(xr_fingerprint_equal(view.baseline.target_plan_fingerprint,
                                 target_before));
    REQUIRE(xr_fingerprint_equal(view.baseline.target_profile_fingerprint,
                                 profile_before));
    REQUIRE(view.records[0].decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(view.records[0].diagnostic_issue ==
            XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED);
    REQUIRE(view.records[0].direct_call.target_call_index == 0);
    /* A refusal keeps no derived facts beyond the row it names. */
    REQUIRE(view.records[0].direct_call_binding.callee_function == 0);
    REQUIRE(view.records[0].direct_call_binding.target_call_index == 0);
    REQUIRE(memcmp(&view.records[0].input_state,
                   &view.records[0].output_state,
                   sizeof(view.records[0].input_state)) == 0);
    /* A fully completed baseline does publish call-shape evidence; the
     * refusal above is a proof failure, not a missing-evidence failure. */
    REQUIRE((view.initial_state.available &
             XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET)) != 0);
    REQUIRE(xr_aot_refinement_verify(&view, fixture.target_plan, &diag));
    REQUIRE(xr_fingerprint_equal(target_before,
                                 xr_target_plan_fingerprint(fixture.target_plan)));
    REQUIRE(xr_fingerprint_equal(profile_before,
                                 xr_target_profile_fingerprint(fixture.target_profile)));

    xr_aot_refinement_plan_free(plan);
    source_namespace_storage_fixture_free(&fixture);
}

/* The whole-plan authority is the production entry point. A closed local
 * callee must be proved, and the derived record must match the baseline. */
static void test_direct_call_authority_applies_closed_local_binding(void) {
    DirectLocalCalleeStorageFixture fixture =
        direct_local_callee_storage_fixture_create(false, false);
    XrFingerprint target_before =
        xr_target_plan_fingerprint(fixture.target_plan);
    uint32_t call_count = 0;
    (void) xr_target_plan_calls(fixture.target_plan, &call_count);
    REQUIRE(call_count == 1);
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_direct_call_authority_build(
        fixture.target_plan, 27902, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified);
    /* Coverage is total: one record per call row, no silent omission. */
    REQUIRE(view.record_count == call_count);
    const XrAotDirectCallRecord *binding = &view.records[0].direct_call_binding;
    REQUIRE(view.records[0].decision == XR_AOT_REFINEMENT_APPLIED);
    REQUIRE(view.records[0].diagnostic_issue == XR_AOT_REFINEMENT_OK);
    REQUIRE(binding->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
    REQUIRE(binding->semantic_target_kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL);
    REQUIRE(binding->argument_count == binding->parameter_count);
    REQUIRE(binding->environment_required == 0);
    REQUIRE(binding->generation_required == 0);
    REQUIRE(!fingerprint_is_zero_bytes(&binding->fingerprint));
    /* Proving a binding must not disturb the baseline it was proved against. */
    REQUIRE(xr_fingerprint_equal(target_before,
                                 xr_target_plan_fingerprint(fixture.target_plan)));
    REQUIRE(xr_aot_refinement_verify(&view, fixture.target_plan, &diag));

    xr_aot_refinement_plan_free(plan);
    direct_local_callee_storage_fixture_free(&fixture);
}

static void test_tail_call_backend_conformance_is_exact(void) {
    DirectLocalCalleeStorageFixture fixture =
        direct_local_callee_storage_fixture_create(false, true);
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_direct_call_authority_build(
        fixture.target_plan, 27903, &plan, &refinement_diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 1);
    XrAotTailCallConformance first = {0};
    XrAotTailCallConformance second = {0};
    XrAotTailCallDiagnostic diag = {0};
    bool verified = xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &first, &diag);
    if (!verified)
        fprintf(stderr,
                "tail conformance failed: %s op=%u call=%u function=%u value=%u\n",
                xr_aot_tail_call_conformance_issue_name(diag.issue),
                diag.semantic_operation, diag.target_call_index,
                diag.semantic_function, diag.semantic_value);
    REQUIRE(verified);
    REQUIRE(first.tail_call_count == 1 &&
            diag.issue == XR_AOT_TAIL_CALL_CONFORMANCE_OK &&
            !fingerprint_is_zero_bytes(&first.fingerprint));
    REQUIRE(xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));
    REQUIRE(memcmp(&first, &second, sizeof(first)) == 0);

    uint16_t saved_opcode = fixture.call->op;
    fixture.call->op = XI_CALL;
    REQUIRE(!xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));
    REQUIRE(diag.issue == XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE);
    fixture.call->op = saved_opcode;

    XiValue *saved_callee = fixture.call->args[0];
    fixture.call->args[0] = fixture.call;
    REQUIRE(!xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));
    REQUIRE(diag.issue == XR_AOT_TAIL_CALL_CONFORMANCE_OPERAND_MAPPING);
    fixture.call->args[0] = saved_callee;

    XiFunc *saved_shared_callee = fixture.function->shared_slot_funcs[0];
    fixture.function->shared_slot_funcs[0] = fixture.decoy;
    REQUIRE(!xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));
    REQUIRE(diag.issue == XR_AOT_TAIL_CALL_CONFORMANCE_CALLEE_MAPPING);
    fixture.function->shared_slot_funcs[0] = saved_shared_callee;

    uint32_t saved_function_index =
        fixture.caller->semantic_plan_function_index;
    fixture.caller->semantic_plan_function_index =
        fixture.decoy->semantic_plan_function_index;
    REQUIRE(!xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));
    REQUIRE(diag.issue == XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY);
    fixture.caller->semantic_plan_function_index = saved_function_index;
    REQUIRE(xr_aot_tail_call_conformance_verify(
        fixture.function, fixture.target_plan, &view, &second, &diag));

    xr_aot_refinement_plan_free(plan);
    direct_local_callee_storage_fixture_free(&fixture);
}

/* Every field the validator derives must be re-derived by the independent
 * verifier. Each mutation below rewrites one proven fact in an APPLIED record
 * and must be rejected; otherwise the record is decoration, not a proof. */
static void test_direct_call_binding_mutations_fail_closed(void) {
    DirectLocalCalleeStorageFixture fixture =
        direct_local_callee_storage_fixture_create(false, false);
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_direct_call_authority_build(
        fixture.target_plan, 27902, &plan, &diag));
    XrAotRefinementPlanView original = xr_aot_refinement_plan_view(plan);
    REQUIRE(original.record_count == 1);
    REQUIRE(original.records[0].decision == XR_AOT_REFINEMENT_APPLIED);
    XrAotTransformationRecord mutated_record = original.records[0];
    XrAotRefinementPlanView mutated = original;
    mutated.records = &mutated_record;
    REQUIRE(xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));

#define REQUIRE_BINDING_MUTATION_REJECTED(field_mutation, expected_issue)      \
    do {                                                                      \
        mutated_record = original.records[0];                                 \
        field_mutation;                                                       \
        REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan,       \
                                          &diag));                            \
        REQUIRE(diag.issue == (uint32_t) (expected_issue));                    \
    } while (0)

    /* Callee rebinding: the single most dangerous direct-call defect. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.callee_function++,
        XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.callee_identity.bytes[0] ^= 1u,
        XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY);
    /* Argument-count and argument-map drift. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.argument_count++,
        XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.parameter_count++,
        XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.argument_map_fingerprint.bytes[0] ^=
            1u,
        XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING);
    /* Error, environment and generation edges. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.error_slot = 0,
        XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.environment_required = 1,
        XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.generation_required = 1,
        XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING);
    /* Result mapping and call classification. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.result_ownership =
            XR_TARGET_CALL_MOVE,
        XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.target_kind =
            XR_TARGET_CALL_TARGET_SOURCE_EXPORT,
        XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.call_flags |=
            XR_TARGET_CALL_GENERATION,
        XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING);
    /* Downgrading a proven binding to a refusal, or forging the record
     * fingerprint, must both be caught. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.decision = XR_AOT_REFINEMENT_REFUSED,
        XR_AOT_REFINEMENT_PLAN_STATE);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.diagnostic_issue =
            XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED,
        XR_AOT_REFINEMENT_PLAN_STATE);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.fingerprint.bytes[0] ^= 1u,
        XR_AOT_REFINEMENT_RECORD_FINGERPRINT);
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.fingerprint.bytes[0] ^= 1u,
        XR_AOT_REFINEMENT_RECORD_FINGERPRINT);
    /* The request key and the derived record must name the same call row. */
    REQUIRE_BINDING_MUTATION_REJECTED(
        mutated_record.direct_call_binding.target_call_index = 7,
        XR_AOT_REFINEMENT_PLAN_STATE);

#undef REQUIRE_BINDING_MUTATION_REJECTED

    xr_aot_refinement_plan_free(plan);
    direct_local_callee_storage_fixture_free(&fixture);
}

/* Naming a call row that does not exist is a caller fault, not a refusal:
 * the protocol must fail rather than record an unprovable binding. */
static void test_direct_call_out_of_range_request_fails_closed(void) {
    RefinementFixture fixture = fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol = xr_aot_refinement_direct_call_protocol(27901);
    XrAotDirectCallRequest request = {.target_call_index = 0};
    uint32_t decision = 0;
    REQUIRE(!xr_aot_refinement_try_direct_call(
        builder, &protocol, fixture.target_plan, &request, &decision, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INVALID_ARGUMENT);
    xr_aot_refinement_builder_free(builder);

    /* A callless plan still yields a verified, empty authority. */
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_direct_call_authority_build(
        fixture.target_plan, 27902, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xr_aot_refinement_plan_free(plan);
    fixture_free(&fixture);
}

static void test_stale_state_and_baseline_mutations_fail_closed(void) {
    SourceNamespaceStorageFixture fixture =
        source_namespace_storage_fixture_create(false, false);
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_refused_plan(fixture.target_plan, &diag);
    XrAotRefinementPlanView original = xr_aot_refinement_plan_view(plan);
    XrAotRefinementPlanView mutated_view = original;
    XrAotTransformationRecord mutated_record = original.records[0];
    mutated_view.records = &mutated_record;

    mutated_record.input_state.generation[XR_AOT_INV_CALL_TARGET]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);
    REQUIRE(diag.record_index == 0);

    mutated_record = original.records[0];
    mutated_record.output_state.generation[XR_AOT_INV_VALUES]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    mutated_record = original.records[0];
    mutated_record.protocol.requires &=
        ~XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET);
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PASS_PROTOCOL);

    mutated_view = original;
    mutated_view.baseline.semantic_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    mutated_view = original;
    mutated_view.baseline.target_plan_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    mutated_view = original;
    mutated_view.baseline.target_profile_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    /* Any family bit outside the required set must invalidate the baseline;
     * pick the first bit above the required mask so this stays correct as
     * families are added. */
    mutated_view = original;
    mutated_view.baseline.completed_family_mask |=
        (uint64_t) (XR_TARGET_REQUIRED_FAMILIES + 1u);
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    xr_aot_refinement_plan_free(plan);
    source_namespace_storage_fixture_free(&fixture);
}

static XrAotRefinementPlan *build_representation_plan(
    const RepresentationFixture *fixture, XrAotRefinementDiagnostic *diag) {
    XrAotRefinementPlan *plan = NULL;
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture->target_plan, &policy, &plan, diag));
    REQUIRE(plan != NULL);
    return plan;
}

static void test_immutable_authority_matches_backend_materialization(void) {
    MaterializationFixture fixture = materialization_fixture_create();
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    policy.force_phi_tagged = true;
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    REQUIRE(plan != NULL);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 3);

    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    if (!xr_aot_representation_materialization_verify(
            &view, fixture.function, fixture.target_plan, &policy, &diag)) {
        fprintf(stderr,
                "materialization verification failed: issue=%s record=%u value=%u operation=%u\n",
                xr_aot_refinement_issue_name(diag.issue), diag.record_index,
                diag.semantic_value, diag.semantic_operation);
        abort();
    }

    const XrAotRepresentationAdapterRecord *first_adapter =
        &view.records[0].representation_adapter;
    XrAotRefinementBuilder *partial_builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(partial_builder != NULL);
    XrAotPassProtocol protocol =
        xr_aot_refinement_representation_protocol(27902);
    XrAotRepresentationAdapterRequest partial_request = {
        .source_value = first_adapter->source_value,
        .use_operation = first_adapter->use_operation,
        .use_block = first_adapter->use_block,
        .use_operand = first_adapter->use_operand,
        .use_kind = first_adapter->use_kind,
        .adapter_kind = first_adapter->adapter_kind,
        .input_rep_kind = first_adapter->input_rep_kind,
        .output_rep_kind = first_adapter->output_rep_kind,
        .layout = first_adapter->layout,
        .policy_fingerprint = first_adapter->policy_fingerprint,
    };
    uint32_t decision = XR_AOT_REFINEMENT_REFUSED;
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        partial_builder, &protocol, fixture.target_plan, &partial_request,
        &decision, &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_APPLIED);
    XrAotRefinementPlan *partial_plan = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(
        partial_builder, fixture.target_plan, &partial_plan, &diag));
    XrAotRefinementPlanView partial_view =
        xr_aot_refinement_plan_view(partial_plan);
    REQUIRE(!xr_aot_representation_materialization_verify(
        &partial_view, fixture.function, fixture.target_plan, &policy,
        &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE);
    xr_aot_refinement_plan_free(partial_plan);
    xr_aot_refinement_builder_free(partial_builder);

    XiValue *then_box = fixture.phi->value.args[0];
    XiValue *else_box = fixture.phi->value.args[1];
    XiValue *phi_unbox = fixture.sum->args[0];
    REQUIRE(then_box && else_box && phi_unbox);
    REQUIRE(then_box->op == XI_BOX &&
            then_box->backend_origin == XI_BACKEND_VALUE_REP_BOX);
    REQUIRE(else_box->op == XI_BOX &&
            else_box->backend_origin == XI_BACKEND_VALUE_REP_BOX);
    REQUIRE(phi_unbox->op == XI_UNBOX &&
            phi_unbox->backend_origin == XI_BACKEND_VALUE_REP_UNBOX);

    fixture.sum->aux_int++;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    fixture.sum->aux_int--;

    fixture.phi->value.args[0] = fixture.then_value;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    fixture.phi->value.args[0] = then_box;

    then_box->args[0] = fixture.else_value;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    then_box->args[0] = fixture.then_value;

    uint8_t unbox_rep = phi_unbox->rep;
    phi_unbox->rep = XR_REP_TAGGED;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    phi_unbox->rep = unbox_rep;

    uint8_t box_origin = then_box->backend_origin;
    then_box->backend_origin = XI_BACKEND_VALUE_REP_UNBOX;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    then_box->backend_origin = box_origin;

    XiRepPolicy stale_policy = policy;
    stale_policy.force_return_tagged = true;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &stale_policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);

    XrAotTransformationRecord reordered[3];
    memcpy(reordered, view.records, sizeof(reordered));
    XrAotTransformationRecord first = reordered[0];
    reordered[0] = reordered[1];
    reordered[1] = first;
    XrAotRefinementPlanView reordered_view = view;
    reordered_view.records = reordered;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &reordered_view, fixture.function, fixture.target_plan, &policy,
        &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_NONCANONICAL_ORDER);

    XiFunc duplicate_index_child = {0};
    duplicate_index_child.semantic_plan = fixture.function->semantic_plan;
    duplicate_index_child.semantic_plan_function_index =
        fixture.function->semantic_plan_function_index;
    XiFunc *duplicate_children[1] = {&duplicate_index_child};
    XiFunc **saved_children = fixture.function->children;
    uint16_t saved_child_count = fixture.function->nchildren;
    fixture.function->children = duplicate_children;
    fixture.function->nchildren = 1;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    fixture.function->children = saved_children;
    fixture.function->nchildren = saved_child_count;

    XiValue *extra = xi_value_new(fixture.function, fixture.then_block,
                                  XI_BOX, &scalar_int, 1);
    REQUIRE(extra != NULL);
    extra->args[0] = fixture.then_value;
    extra->rep = XR_REP_TAGGED;
    extra->backend_origin = XI_BACKEND_VALUE_REP_BOX;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE);

    xr_aot_refinement_plan_free(plan);
    materialization_fixture_free(&fixture);
}

static void test_scalar_shared_boundary_is_exact_and_fail_closed(void) {
    SharedScalarFixture fixture = shared_scalar_fixture_create();
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 1);
    const XrAotRepresentationAdapterRecord *adapter =
        &view.records[0].representation_adapter;
    REQUIRE(adapter->adapter_kind == XR_AOT_REP_ADAPTER_BOX);
    REQUIRE(adapter->input_rep_kind == XR_MACHINE_REP_I64);
    REQUIRE(adapter->output_rep_kind == XR_MACHINE_REP_DYN_VALUE);
    const XrSemanticOperationRecord *use = xr_semantic_plan_operation(
        fixture.function->semantic_plan, adapter->use_operation);
    REQUIRE(use != NULL && use->opcode == XI_SET_SHARED);

    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.store->args[0] != NULL &&
            fixture.store->args[0]->op == XI_BOX &&
            fixture.store->args[0]->backend_origin ==
                XI_BACKEND_VALUE_REP_BOX);
    REQUIRE(fixture.store->args[0]->args[0] == fixture.constant);
    REQUIRE(fixture.print->args[0] == fixture.load);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    XiValue *box = fixture.store->args[0];
    fixture.store->args[0] = fixture.constant;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    fixture.store->args[0] = box;

    box->args[0] = fixture.load;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);
    box->args[0] = fixture.constant;

    fixture.store->aux_int++;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    fixture.store->aux_int--;

    xr_aot_refinement_plan_free(plan);
    shared_scalar_fixture_free(&fixture);

    XiFunc *string_function = xi_func_new("shared_string_representation",
                                          &scalar_unit);
    REQUIRE(string_function != NULL);
    XiBlock *entry = xi_block_new(string_function);
    XiValue *text = xi_const_str(string_function, entry, "not-scalar",
                                 &scalar_string);
    XiValue *store = xi_value_new(string_function, entry, XI_SET_SHARED,
                                  &scalar_unit, 1);
    REQUIRE(entry && text && store);
    store->args[0] = text;
    xi_block_set_return(entry, NULL);
    string_function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(string_function, error,
                                              sizeof(error)));
    XrTargetProfile *profile = build_target_profile();
    XrTargetPlan *target_plan = NULL;
    REQUIRE(xr_target_plan_build(string_function->semantic_plan, profile,
                                 &target_plan, error, sizeof(error)));
    plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target_plan, &policy, &plan, &diag));
    view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xi_opt_refresh_representations_with_policy(string_function, &policy);
    REQUIRE(store->args[0] == text && text->rep == XR_REP_TAGGED);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, string_function, target_plan, &policy, &diag));
    xr_aot_refinement_plan_free(plan);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    xi_func_free(string_function);
}

static void test_exact_heap_closure_storage_is_tagged_and_fail_closed(void) {
    ClosureStorageFixture fixture = closure_storage_fixture_create(false);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);

    uint32_t closure_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < xr_semantic_plan_operation_count(fixture.function->semantic_plan);
         i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(
            fixture.function->semantic_plan, i);
        if (operation && operation->opcode == XI_CLOSURE_NEW) {
            REQUIRE(closure_value == XR_SEMANTIC_INDEX_NONE &&
                    operation->operand_count == 0);
            closure_value = operation->result_value;
        }
    }
    REQUIRE(closure_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(fixture.target_plan, closure_value);
    REQUIRE(binding != NULL);
    const XrTargetMachineRepRecord *register_rep = xr_target_plan_machine_rep(
        fixture.target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep = xr_target_plan_machine_rep(
        fixture.target_plan, binding->memory_rep);
    REQUIRE(register_rep != NULL && memory_rep != NULL &&
            register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            register_rep->ownership == XR_TARGET_OWNERSHIP_OWNED);

    XiValue *closure_before = fixture.closure;
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.store->args[0] == closure_before &&
            fixture.store->args[0]->op == XI_CLOSURE_NEW &&
            fixture.store->args[0]->rep == XR_REP_TAGGED &&
            fixture.store->args[0]->backend_origin == XI_BACKEND_VALUE_NONE);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    xr_aot_refinement_plan_free(plan);
    closure_storage_fixture_free(&fixture);

    fixture = closure_storage_fixture_create(true);
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    REQUIRE(plan == NULL &&
            diag.issue ==
                XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE);
    closure_storage_fixture_free(&fixture);
}

static void test_direct_local_shared_callee_storage_is_exact_and_fail_closed(void) {
    DirectLocalCalleeStorageFixture fixture =
        direct_local_callee_storage_fixture_create(false, false);
    REQUIRE((fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE) != 0);
    uint32_t load_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t load_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < xr_semantic_plan_operation_count(fixture.function->semantic_plan);
         i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(fixture.function->semantic_plan, i);
        if (operation && operation->opcode == XI_GET_SHARED) {
            REQUIRE(load_operation == XR_SEMANTIC_INDEX_NONE);
            load_operation = i;
            load_value = operation->result_value;
        }
    }
    REQUIRE(load_operation != XR_SEMANTIC_INDEX_NONE &&
            load_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(fixture.target_plan, load_value);
    REQUIRE(binding != NULL);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(fixture.target_plan,
                                   binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(fixture.target_plan,
                                   binding->memory_rep);
    REQUIRE(register_rep && memory_rep &&
            register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            register_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            memory_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC);
    REQUIRE(binding->slot < fixture.target_plan->slots_count);
    const XrTargetSlotRecord *slot =
        &fixture.target_plan->slots[binding->slot];
    REQUIRE(slot->semantic_value == load_value &&
            slot->semantic_operation == load_operation &&
            slot->role == XR_TARGET_SLOT_TEMPORARY &&
            slot->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            slot->root_kind == XR_TARGET_ROOT_DYNAMIC);

    REQUIRE(fixture.target_plan->calls_count == 1);
    XrTargetCallRecord saved_call = fixture.target_plan->calls[0];
    XrFingerprint saved_call_plan_fingerprint =
        fixture.target_plan->fingerprint;
    fixture.target_plan->calls[0].callee_function =
        fixture.decoy->semantic_plan_function_index;
    xr_target_call_compute_fingerprint(fixture.target_plan, 0,
                                       &fixture.target_plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    char error[512] = {0};
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->calls[0] = saved_call;
    fixture.target_plan->fingerprint = saved_call_plan_fingerprint;

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile),
        &emission, error, sizeof(error)));
    XrCValueEmissionView emission_value = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, load_value,
                                          &emission_value, error,
                                          sizeof(error)));
    REQUIRE(emission_value.rep == XR_C_VALUE_REP_TAGGED &&
            emission_value.materialization ==
                XR_C_VALUE_MATERIALIZATION_NONE &&
            strcmp(emission_value.c_type, "XrValue") == 0 &&
            emission_value.literal_bytes == NULL &&
            emission_value.literal_byte_length == 0);

    uint32_t direct_row_index = UINT32_MAX;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == load_value) {
            REQUIRE(direct_row_index == UINT32_MAX);
            direct_row_index = i;
        }
    REQUIRE(direct_row_index != UINT32_MAX);
    uint32_t saved_emission_count = emission->value_count;
    XrCValueEmissionView *row_snapshot =
        (XrCValueEmissionView *) xr_calloc(saved_emission_count,
                                           sizeof(*row_snapshot));
    REQUIRE(row_snapshot != NULL);
    memcpy(row_snapshot, emission->values,
           saved_emission_count * sizeof(*row_snapshot));
    memmove(&emission->values[direct_row_index],
            &emission->values[direct_row_index + 1u],
            (saved_emission_count - direct_row_index - 1u) *
                sizeof(*emission->values));
    emission->value_count--;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->value_count = saved_emission_count;
    memcpy(emission->values, row_snapshot,
           saved_emission_count * sizeof(*row_snapshot));
    xr_free(row_snapshot);

    XrCValueEmissionView *saved_rows = emission->values;
    XrCValueEmissionView *extra_rows =
        (XrCValueEmissionView *) xr_calloc(saved_emission_count + 1u,
                                           sizeof(*extra_rows));
    REQUIRE(extra_rows != NULL);
    memcpy(extra_rows, saved_rows,
           saved_emission_count * sizeof(*extra_rows));
    extra_rows[saved_emission_count] =
        saved_rows[direct_row_index];
    extra_rows[saved_emission_count].semantic_value = UINT32_MAX;
    emission->values = extra_rows;
    emission->value_count++;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->value_count = saved_emission_count;
    emission->values = saved_rows;
    xr_free(extra_rows);

    XrCValueEmissionView *direct_row =
        &emission->values[direct_row_index];
    direct_row->target_register_kind = XR_MACHINE_REP_I64;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    direct_row->target_register_kind = XR_MACHINE_REP_DYN_VALUE;
    direct_row->rep = XR_C_VALUE_REP_I64;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    direct_row->rep = XR_C_VALUE_REP_TAGGED;
    direct_row->c_type = "int64_t";
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    direct_row->c_type = "XrValue";
    direct_row->materialization =
        XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    direct_row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    emission->profile_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->profile_fingerprint.bytes[0] ^= 1u;
    emission->target_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->target_fingerprint.bytes[0] ^= 1u;
    emission->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->fingerprint.bytes[0] ^= 1u;
    REQUIRE(xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    xr_c_emission_plan_free(emission);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.load->rep == XR_REP_TAGGED &&
            fixture.load->backend_origin == XI_BACKEND_VALUE_NONE &&
            fixture.call->args[0] == fixture.load);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    XiFunc *saved_parent = fixture.child->parent_func;
    fixture.child->parent_func = fixture.caller;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY ||
            diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    fixture.child->parent_func = saved_parent;

    uint32_t saved_child_index =
        fixture.child->semantic_plan_function_index;
    fixture.child->semantic_plan_function_index =
        fixture.decoy->semantic_plan_function_index;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY ||
            diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    fixture.child->semantic_plan_function_index = saved_child_index;

    fixture.function->shared_slot_funcs[0] = fixture.decoy;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY ||
            diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    fixture.function->shared_slot_funcs[0] = fixture.child;

    XiValue *saved_callee = fixture.call->args[0];
    fixture.call->args[0] = fixture.call;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    fixture.call->args[0] = saved_callee;
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    uint64_t saved_mask = fixture.target_plan->completed_family_mask;
    XrFingerprint saved_fingerprint = fixture.target_plan->fingerprint;
    fixture.target_plan->completed_family_mask &=
        ~XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->completed_family_mask = saved_mask;
    fixture.target_plan->fingerprint = saved_fingerprint;

    uint32_t machine_rep_index = binding->register_rep;
    uint16_t saved_ownership =
        fixture.target_plan->machine_reps[machine_rep_index].ownership;
    fixture.target_plan->machine_reps[machine_rep_index].ownership =
        XR_TARGET_OWNERSHIP_OWNED;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->machine_reps[machine_rep_index].ownership =
        saved_ownership;
    fixture.target_plan->fingerprint = saved_fingerprint;

    xr_aot_refinement_plan_free(plan);
    direct_local_callee_storage_fixture_free(&fixture);

    DirectLocalCalleeStorageFixture extra_use =
        direct_local_callee_storage_fixture_create(true, false);
    direct_local_callee_storage_fixture_free(&extra_use);
}

static void test_direct_local_tagged_ref_argument_authority_is_exact(void) {
    DirectLocalTaggedRefFixture fixture =
        direct_local_tagged_ref_fixture_create();
    XrSemanticPlan *semantic = fixture.function->semantic_plan;
    XrTargetPlan *target = fixture.target_plan;
    char error[512] = {0};
    REQUIRE((target->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE) != 0);
    REQUIRE(target->calls_count == 1 && target->call_arguments_count == 1);
    const XrTargetCallRecord *call = &target->calls[0];
    XrTargetCallArgumentRecord *argument = &target->call_arguments[0];
    const XrSemanticOperationRecord *call_operation =
        xr_semantic_plan_operation(semantic, call->semantic_operation);
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, argument->callee_parameter);
    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, call->semantic_call_target);
    uint32_t place_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode != XI_LOCAL_ADDR ||
            operation->function != call->caller_function)
            continue;
        REQUIRE(place_value == XR_SEMANTIC_INDEX_NONE);
        place_value = operation->result_value;
    }
    REQUIRE(call_operation && parameter && semantic_target &&
            call_operation->result_value != XR_SEMANTIC_INDEX_NONE &&
            place_value != XR_SEMANTIC_INDEX_NONE);
    REQUIRE(argument->call == call->id && argument->ordinal == 0 &&
            argument->semantic_value == place_value &&
            argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_BORROW &&
            argument->transfer_mode == XR_TRANSFER_SHARE &&
            argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_U8 &&
            argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
            argument->reserved8[2] == 0);
    REQUIRE(target->machine_reps[argument->register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            target->machine_reps[argument->memory_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            target->machine_reps[argument->callee_register_rep].kind ==
                XR_MACHINE_REP_RAW_PTR &&
            target->machine_reps[argument->callee_memory_rep].kind ==
                XR_MACHINE_REP_RAW_PTR);
    const XrTargetValueRepRecord *parameter_binding =
        xr_target_plan_value_rep(target, parameter->value);
    REQUIRE(parameter_binding &&
            target->machine_reps[parameter_binding->register_rep].kind ==
                XR_MACHINE_REP_RAW_PTR &&
            target->machine_reps[parameter_binding->memory_rep].kind ==
                XR_MACHINE_REP_RAW_PTR &&
            target->machine_reps[parameter_binding->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_BORROWED);

    XrStableId expected_v2_identity = {{0}};
    XrStableId legacy_v1_identity = {{0}};
    REQUIRE(test_pair_identity("xray-target-direct-tagged-ref-argument-v2",
                               semantic_target->id, parameter->id, argument->ordinal,
                               &expected_v2_identity));
    REQUIRE(test_pair_identity("xray-target-direct-array-ref-argument-" "v1",
                               semantic_target->id, parameter->id, argument->ordinal,
                               &legacy_v1_identity));
    REQUIRE(xr_stable_id_equal(argument->identity, expected_v2_identity));
    REQUIRE(!xr_stable_id_equal(argument->identity, legacy_v1_identity));

    XrTargetCallArgumentRecord saved_argument = *argument;
    XrTargetCallRecord saved_call = target->calls[0];
    XrFingerprint saved_target_fingerprint = target->fingerprint;
    argument->identity = legacy_v1_identity;
    xr_target_call_compute_fingerprint(target, 0, &target->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    XrCEmissionPlan *legacy_rejected = NULL;
    REQUIRE(!xr_c_emission_plan_build(
        target, xr_target_profile_fingerprint(fixture.target_profile), &legacy_rejected, error,
        sizeof(error)));
    REQUIRE(legacy_rejected == NULL && strstr(error, "tagged ref argument") != NULL);
    *argument = saved_argument;
    target->calls[0] = saved_call;
    target->fingerprint = saved_target_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    for (uint32_t mutation = 0; mutation < 14; mutation++) {
        *argument = saved_argument;
        switch (mutation) {
            case 0: argument->semantic_operand++; break;
            case 1: argument->semantic_value = parameter->value; break;
            case 2: argument->callee_parameter++; break;
            case 3: argument->caller_slot = argument->callee_slot; break;
            case 4: argument->callee_slot = argument->caller_slot; break;
            case 5: argument->register_rep = argument->callee_register_rep; break;
            case 6: argument->callee_memory_rep = argument->memory_rep; break;
            case 7: argument->mode = XR_TARGET_CALL_VALUE; break;
            case 8: argument->ownership = XR_TARGET_CALL_READ; break;
            case 9: argument->transfer_mode = XR_TRANSFER_MOVE; break;
            case 10: argument->flags = 0; break;
            case 11:
                argument->array_element_storage = XR_TARGET_ARRAY_STORAGE_I64;
                break;
            case 12: argument->reserved8[0] = 1; break;
            case 13: argument->identity.bytes[0] ^= 1u; break;
            default: abort();
        }
        xr_target_call_compute_fingerprint(target, 0,
                                           &target->calls[0].fingerprint);
        xr_target_plan_compute_fingerprint(target, &target->fingerprint);
        if (mutation == 13) {
            XrCEmissionPlan *rejected = NULL;
            error[0] = '\0';
            REQUIRE(!xr_c_emission_plan_build(
                target, xr_target_profile_fingerprint(fixture.target_profile),
                &rejected, error, sizeof(error)));
            REQUIRE(rejected == NULL &&
                    strstr(error, "direct-local tagged ref argument") != NULL);
        }
        REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    }
    *argument = saved_argument;
    target->calls[0] = saved_call;
    target->fingerprint = saved_target_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *direct_plan = NULL;
    REQUIRE(xr_aot_refinement_direct_call_authority_build(
        target, 27902, &direct_plan, &diag));
    XrAotRefinementPlanView direct_view =
        xr_aot_refinement_plan_view(direct_plan);
    REQUIRE(direct_view.record_count == 1 && direct_view.verified &&
            direct_view.records[0].decision == XR_AOT_REFINEMENT_APPLIED &&
            direct_view.records[0].direct_call_binding.argument_count == 1 &&
            !fingerprint_is_zero_bytes(
                &direct_view.records[0].direct_call_binding
                     .argument_map_fingerprint));
    XrAotTransformationRecord mutated_record = direct_view.records[0];
    XrAotRefinementPlanView mutated_direct = direct_view;
    mutated_direct.records = &mutated_record;
    mutated_record.direct_call_binding.argument_map_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_direct, target, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING);
    xr_aot_refinement_plan_free(direct_plan);

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        target, xr_target_profile_fingerprint(fixture.target_profile),
        &emission, error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_call_argument_count(emission) == 1);
    XrCCallArgumentEmissionView call_view = {0};
    REQUIRE(xr_c_emission_plan_call_argument_view(
        emission, call_operation->result_value, 0, &call_view, error,
        sizeof(error)));
    REQUIRE(call_view.semantic_operand == argument->semantic_operand &&
            call_view.semantic_value == argument->semantic_value &&
            call_view.callee_parameter == argument->callee_parameter &&
            call_view.caller_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            call_view.caller_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            call_view.callee_register_kind == XR_MACHINE_REP_RAW_PTR &&
            call_view.callee_memory_kind == XR_MACHINE_REP_RAW_PTR &&
            call_view.mode == XR_TARGET_CALL_REFERENCE &&
            call_view.ownership == XR_TARGET_CALL_BORROW &&
            call_view.transfer_mode == XR_TRANSFER_SHARE &&
            call_view.flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            call_view.array_element_storage == XR_TARGET_ARRAY_STORAGE_U8 &&
            strcmp(call_view.c_type, "XrValue *") == 0);
    XrCValueEmissionView parameter_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
        emission, parameter->value, &parameter_view, error, sizeof(error)));
    REQUIRE(parameter_view.rep == XR_C_VALUE_REP_RAW_PTR &&
            parameter_view.materialization ==
                XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_TAGGED_REF_PARAMETER &&
            parameter_view.recipe_discriminant ==
                XR_TARGET_ARRAY_STORAGE_U8 &&
            strcmp(parameter_view.c_type, "XrValue *") == 0);

    uint32_t parameter_emission_index = UINT32_MAX;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == parameter->value) {
            REQUIRE(parameter_emission_index == UINT32_MAX);
            parameter_emission_index = i;
        }
    REQUIRE(parameter_emission_index != UINT32_MAX);
    uint32_t saved_parameter_storage =
        emission->values[parameter_emission_index].recipe_discriminant;
    emission->values[parameter_emission_index].recipe_discriminant =
        XR_TARGET_ARRAY_STORAGE_I64;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->values[parameter_emission_index].recipe_discriminant =
        saved_parameter_storage;
    REQUIRE(xr_c_emission_plan_verify(
        emission, target,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));

    XrCCallArgumentEmissionView saved_emission_argument =
        emission->call_arguments[0];
    for (uint32_t mutation = 0; mutation < 13; mutation++) {
        emission->call_arguments[0] = saved_emission_argument;
        XrCCallArgumentEmissionView *row = &emission->call_arguments[0];
        switch (mutation) {
            case 0: row->semantic_call_value++; break;
            case 1: row->semantic_operand++; break;
            case 2: row->semantic_value = parameter->value; break;
            case 3: row->callee_parameter++; break;
            case 4: row->ordinal++; break;
            case 5: row->caller_register_kind = XR_MACHINE_REP_RAW_PTR; break;
            case 6: row->callee_memory_kind = XR_MACHINE_REP_DYN_VALUE; break;
            case 7: row->mode = XR_TARGET_CALL_VALUE; break;
            case 8: row->ownership = XR_TARGET_CALL_READ; break;
            case 9: row->transfer_mode = XR_TRANSFER_MOVE; break;
            case 10: row->flags = 0; break;
            case 11:
                row->array_element_storage = XR_TARGET_ARRAY_STORAGE_I64;
                break;
            case 12: row->c_type = "XrValue"; break;
            default: abort();
        }
        REQUIRE(!xr_c_emission_plan_verify(
            emission, target,
            xr_target_profile_fingerprint(fixture.target_profile), error,
            sizeof(error)));
    }
    emission->call_arguments[0] = saved_emission_argument;
    REQUIRE(xr_c_emission_plan_verify(
        emission, target,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    xr_c_emission_plan_free(emission);

    direct_local_tagged_ref_fixture_free(&fixture);
}

static void test_direct_local_go_callee_storage_is_exact_and_fail_closed(void) {
    DirectLocalGoCalleeStorageFixture fixture =
        direct_local_go_callee_storage_fixture_create(false, false);
    REQUIRE((fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE) != 0);
    REQUIRE((fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE) != 0);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    char error[512] = {0};
    REQUIRE(xr_aot_scalar_semantic_value_id(
        fixture.target_plan, fixture.function, fixture.load,
        &semantic_function, &semantic_value, error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(fixture.target_plan, semantic_value);
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(fixture.target_plan,
                                              binding->register_rep)
                : NULL;
    REQUIRE(binding && machine &&
            machine->kind == XR_MACHINE_REP_DYN_VALUE &&
            machine->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            machine->root_kind == XR_TARGET_ROOT_DYNAMIC);
    uint32_t task_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t task_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < xr_semantic_plan_operation_count(fixture.function->semantic_plan);
         i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(fixture.function->semantic_plan, i);
        if (operation && operation->opcode == XI_GO) {
            REQUIRE(task_value == XR_SEMANTIC_INDEX_NONE);
            task_function = operation->function;
            task_value = operation->result_value;
        }
    }
    REQUIRE(task_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *task_binding =
        xr_target_plan_value_rep(fixture.target_plan, task_value);
    const XrTargetMachineRepRecord *task_machine =
        task_binding ? xr_target_plan_machine_rep(
                           fixture.target_plan, task_binding->register_rep)
                     : NULL;
    REQUIRE(task_binding && task_machine && task_function == semantic_function &&
            task_machine->kind == XR_MACHINE_REP_DYN_VALUE &&
            task_machine->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            task_machine->root_kind == XR_TARGET_ROOT_DYNAMIC);
    uint32_t live_task_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t live_task_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        fixture.target_plan, fixture.function, fixture.go,
        &live_task_function, &live_task_value, error, sizeof(error)));
    REQUIRE(live_task_function == task_function &&
            live_task_value == task_value);

    /* A source class that reuses the Task spelling is not the frozen builtin
     * declaration. The live bridge must reject it even when every scalar
     * representation field remains otherwise identical. */
    XrClassInfo *saved_class_ref = fixture.go->type->instance.class_ref;
    fixture.go->type->instance.class_ref = (XrClassInfo *) fixture.function;
    REQUIRE(!xr_aot_scalar_semantic_value_id(
        fixture.target_plan, fixture.function, fixture.go,
        &live_task_function, &live_task_value, error, sizeof(error)));
    fixture.go->type->instance.class_ref = saved_class_ref;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        fixture.target_plan, fixture.function, fixture.go,
        &live_task_function, &live_task_value, error, sizeof(error)));

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile),
        &emission, error, sizeof(error)));
    XrCValueEmissionView value = {0};
    REQUIRE(xr_c_emission_plan_value_view(
        emission, semantic_value, &value, error, sizeof(error)));
    REQUIRE(value.rep == XR_C_VALUE_REP_TAGGED &&
            value.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            strcmp(value.c_type, "XrValue") == 0);
    REQUIRE(xr_c_emission_plan_value_view(
        emission, task_value, &value, error, sizeof(error)));
    REQUIRE(value.rep == XR_C_VALUE_REP_TAGGED &&
            value.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            strcmp(value.c_type, "XrValue") == 0);
    xr_c_emission_plan_free(emission);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.load->rep == XR_REP_TAGGED &&
            fixture.go->args[0] == fixture.load &&
            fixture.go->rep == XR_REP_TAGGED &&
            fixture.await->args[0] == fixture.go);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    fixture.function->shared_slot_funcs[0] = fixture.decoy;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY &&
            diag.semantic_value == semantic_value);
    fixture.function->shared_slot_funcs[0] = fixture.child;
    XiValue *saved = fixture.go->args[0];
    fixture.go->args[0] = fixture.go;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE &&
            diag.semantic_value == semantic_value);
    fixture.go->args[0] = saved;
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    uint64_t families = fixture.target_plan->completed_family_mask;
    XrFingerprint fingerprint = fixture.target_plan->fingerprint;
    fixture.target_plan->completed_family_mask &=
        ~XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->completed_family_mask = families;
    fixture.target_plan->fingerprint = fingerprint;

    fixture.target_plan->completed_family_mask &=
        ~XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->completed_family_mask = families;
    fixture.target_plan->fingerprint = fingerprint;

    uint32_t machine_rep_index = binding->register_rep;
    uint16_t ownership =
        fixture.target_plan->machine_reps[machine_rep_index].ownership;
    fixture.target_plan->machine_reps[machine_rep_index].ownership =
        XR_TARGET_OWNERSHIP_OWNED;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->machine_reps[machine_rep_index].ownership = ownership;
    fixture.target_plan->fingerprint = fingerprint;

    uint32_t task_machine_rep_index = task_binding->register_rep;
    uint16_t task_ownership =
        fixture.target_plan->machine_reps[task_machine_rep_index].ownership;
    fixture.target_plan->machine_reps[task_machine_rep_index].ownership =
        XR_TARGET_OWNERSHIP_OWNED;
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error,
                                   sizeof(error)));
    fixture.target_plan->machine_reps[task_machine_rep_index].ownership =
        task_ownership;
    fixture.target_plan->fingerprint = fingerprint;

    xr_aot_refinement_plan_free(plan);
    direct_local_go_callee_storage_fixture_free(&fixture);
    DirectLocalGoCalleeStorageFixture extra =
        direct_local_go_callee_storage_fixture_create(true, false);
    direct_local_go_callee_storage_fixture_free(&extra);
    DirectLocalGoCalleeStorageFixture duplicate =
        direct_local_go_callee_storage_fixture_create(false, true);
    direct_local_go_callee_storage_fixture_free(&duplicate);
}

static void test_source_namespace_storage_is_exact_and_fail_closed(void) {
    SourceNamespaceStorageFixture fixture =
        source_namespace_storage_fixture_create(false, false);
    REQUIRE((fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE) != 0);
    uint32_t import_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t load_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < xr_semantic_plan_operation_count(
                 fixture.function->semantic_plan);
         i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(fixture.function->semantic_plan, i);
        if (operation && operation->opcode == XI_IMPORT_REF)
            import_value = operation->result_value;
        else if (operation && operation->opcode == XI_GET_SHARED)
            load_value = operation->result_value;
    }
    REQUIRE(import_value != XR_SEMANTIC_INDEX_NONE &&
            load_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *import_binding =
        xr_target_plan_value_rep(fixture.target_plan, import_value);
    const XrTargetValueRepRecord *load_binding =
        xr_target_plan_value_rep(fixture.target_plan, load_value);
    REQUIRE(import_binding && load_binding &&
            fixture.target_plan->machine_reps[
                import_binding->register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            fixture.target_plan->machine_reps[
                load_binding->register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            fixture.target_plan->machine_reps[
                import_binding->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_BORROWED &&
            fixture.target_plan->machine_reps[
                load_binding->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_BORROWED);

    char error[512] = {0};
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile),
        &emission, error, sizeof(error)));
    XrCValueEmissionView import_view = {0};
    XrCValueEmissionView load_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, import_value, &import_view, error, sizeof(error)) &&
            xr_c_emission_plan_value_view(
                emission, load_value, &load_view, error, sizeof(error)));
    REQUIRE(import_view.rep == XR_C_VALUE_REP_TAGGED &&
            load_view.rep == XR_C_VALUE_REP_TAGGED &&
            import_view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            load_view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            strcmp(import_view.c_type, "XrValue") == 0 &&
            strcmp(load_view.c_type, "XrValue") == 0);
    uint32_t source_row = UINT32_MAX;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == load_value)
            source_row = i;
    REQUIRE(source_row != UINT32_MAX);
    uint16_t saved_kind = emission->values[source_row].target_register_kind;
    emission->values[source_row].target_register_kind = XR_MACHINE_REP_I64;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->values[source_row].target_register_kind = saved_kind;
    const char *saved_c_type = emission->values[source_row].c_type;
    emission->values[source_row].c_type = "int64_t";
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    emission->values[source_row].c_type = saved_c_type;
    uint32_t saved_count = emission->value_count;
    XrCValueEmissionView saved_row = emission->values[source_row];
    memmove(&emission->values[source_row],
            &emission->values[source_row + 1u],
            (saved_count - source_row - 1u) * sizeof(*emission->values));
    emission->value_count--;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    memmove(&emission->values[source_row + 1u],
            &emission->values[source_row],
            (saved_count - source_row - 1u) * sizeof(*emission->values));
    emission->values[source_row] = saved_row;
    emission->value_count = saved_count;
    REQUIRE(xr_c_emission_plan_verify(
        emission, fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), error,
        sizeof(error)));
    xr_c_emission_plan_free(emission);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    bool refined = xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag);
    REQUIRE(refined);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.namespace_ref->rep == XR_REP_TAGGED &&
            fixture.namespace_alias->rep == XR_REP_TAGGED &&
            fixture.receiver->rep == XR_REP_TAGGED &&
            fixture.receiver_alias->rep == XR_REP_TAGGED);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    XiImportRef *live_import = (XiImportRef *) fixture.namespace_ref->aux;
    const char *saved_module_path = live_import->module_path;
    live_import->module_path = "fixture/forged_dependency.xr";
    /* Import spelling is not module identity once resolution has bound the
     * source module. */
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    live_import->module_path = saved_module_path;
    const char *saved_resolved_path = live_import->resolved_module->path;
    live_import->resolved_module->path = "fixture/forged_dependency.xr";
    /* Physical source locators are not durable identity authority. */
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    live_import->resolved_module->path = saved_resolved_path;
    char *saved_resolved_identity = live_import->resolved_module->identity;
    live_import->resolved_module->identity = "module-id-v1:forged";
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY ||
            diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    live_import->resolved_module->identity = saved_resolved_identity;
    int64_t saved_slot = fixture.receiver->aux_int;
    fixture.receiver->aux_int++;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    fixture.receiver->aux_int = saved_slot;
    XiValue *saved_receiver = fixture.call->args[0];
    fixture.call->args[0] = fixture.call;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    fixture.call->args[0] = saved_receiver;
    /* Representation refresh has mechanically propagated and eliminated the
     * frozen identity COPY nodes.  Their exactness is proven by the frozen
     * Target authority; the live verifier instead rejects mutations of the
     * retained producer and consumer edges above. */
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    xr_aot_refinement_plan_free(plan);
    source_namespace_storage_fixture_free(&fixture);
    SourceNamespaceStorageFixture extra =
        source_namespace_storage_fixture_create(true, false);
    source_namespace_storage_fixture_free(&extra);
}

static void test_standalone_source_namespace_storage_is_exact_and_fail_closed(void) {
    SourceNamespaceStorageFixture fixture =
        source_namespace_storage_fixture_create(false, true);
    REQUIRE(xr_semantic_plan_dependency_count(fixture.function->semantic_plan) == 1 &&
            xr_semantic_plan_call_target_count(fixture.function->semantic_plan) == 0 &&
            (fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE) != 0);
    uint32_t import_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < xr_semantic_plan_operation_count(fixture.function->semantic_plan);
         i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(fixture.function->semantic_plan, i);
        if (operation && operation->opcode == XI_IMPORT_REF)
            import_value = operation->result_value;
    }
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(fixture.target_plan, import_value);
    REQUIRE(import_value != XR_SEMANTIC_INDEX_NONE && binding &&
            fixture.target_plan->machine_reps[binding->register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            fixture.target_plan->machine_reps[binding->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_BORROWED);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.namespace_ref->rep == XR_REP_TAGGED &&
            xr_aot_representation_materialization_verify(
                &view, fixture.function, fixture.target_plan, &policy, &diag));

    XiImportRef *live_import = (XiImportRef *) fixture.namespace_ref->aux;
    const char *module_path = live_import->module_path;
    live_import->module_path = "fixture/forged_standalone_dependency.xr";
    /* Standalone package namespaces obey the same resolved identity rule. */
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    live_import->module_path = module_path;
    const char *resolved_path = live_import->resolved_module->path;
    live_import->resolved_module->path =
        "fixture/forged_standalone_dependency.xr";
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    live_import->resolved_module->path = resolved_path;
    char *resolved_identity = live_import->resolved_module->identity;
    live_import->resolved_module->identity = "module-id-v1:forged";
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY ||
            diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    live_import->resolved_module->identity = resolved_identity;
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    xr_aot_refinement_plan_free(plan);
    source_namespace_storage_fixture_free(&fixture);
    SourceNamespaceStorageFixture extra =
        source_namespace_storage_fixture_create(true, true);
    source_namespace_storage_fixture_free(&extra);
}

static void test_native_namespace_yieldable_storage_uses_frozen_call_identity(void) {
    NativeNamespaceYieldableStorageFixture fixture =
        native_namespace_yieldable_storage_fixture_create(false);
    REQUIRE((fixture.target_plan->completed_family_mask &
             XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE) != 0);
    const XrSemanticPlan *semantic = fixture.function->semantic_plan;
    REQUIRE(xr_semantic_plan_call_target_count(semantic) == 1);
    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, 0);
    REQUIRE(semantic_target &&
            semantic_target->kind ==
                XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE);

    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t load_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        fixture.target_plan, fixture.caller, fixture.namespace_load,
        &semantic_function, &load_value, error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(fixture.target_plan, load_value);
    REQUIRE(binding && binding->slot < fixture.target_plan->slots_count);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(fixture.target_plan,
                                   binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(fixture.target_plan, binding->memory_rep);
    const XrTargetSlotRecord *slot =
        &fixture.target_plan->slots[binding->slot];
    REQUIRE(register_rep && memory_rep &&
            register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            register_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            memory_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            slot->semantic_value == load_value &&
            slot->role == XR_TARGET_SLOT_TEMPORARY &&
            slot->ownership == XR_TARGET_OWNERSHIP_BORROWED);

    uint32_t call_index = UINT32_MAX;
    for (uint32_t i = 0; i < fixture.target_plan->calls_count; i++)
        if (fixture.target_plan->calls[i].target_kind ==
            XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE)
            call_index = call_index == UINT32_MAX ? i : UINT32_MAX;
    REQUIRE(call_index != UINT32_MAX &&
            fixture.target_plan->calls[call_index].semantic_call_target == 0 &&
            (fixture.target_plan->calls[call_index].flags &
             XR_TARGET_CALL_SUSPEND) != 0);

    XrTargetCallRecord saved_call = fixture.target_plan->calls[call_index];
    XrFingerprint saved_plan_fingerprint = fixture.target_plan->fingerprint;
    fixture.target_plan->calls[call_index].semantic_call_target =
        XR_SEMANTIC_INDEX_NONE;
    xr_target_call_compute_fingerprint(
        fixture.target_plan, call_index,
        &fixture.target_plan->calls[call_index].fingerprint);
    xr_target_plan_compute_fingerprint(fixture.target_plan,
                                       &fixture.target_plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(fixture.target_plan, error, sizeof(error)));
    fixture.target_plan->calls[call_index] = saved_call;
    fixture.target_plan->fingerprint = saved_plan_fingerprint;
    REQUIRE(xr_target_plan_verify(fixture.target_plan, error, sizeof(error)));

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        fixture.target_plan,
        xr_target_profile_fingerprint(fixture.target_profile), &emission,
        error, sizeof(error)));
    XrCValueEmissionView load_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
        emission, load_value, &load_view, error, sizeof(error)));
    REQUIRE(load_view.rep == XR_C_VALUE_REP_TAGGED &&
            load_view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            strcmp(load_view.c_type, "XrValue") == 0);
    xr_c_emission_plan_free(emission);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    bool refined = xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag);
    if (!refined)
        fprintf(stderr,
                "native namespace refinement failed: issue=%u value=%u operation=%u\n",
                (unsigned) diag.issue, diag.semantic_value,
                diag.semantic_operation);
    REQUIRE(refined);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(fixture.namespace_ref->rep == XR_REP_TAGGED &&
            fixture.namespace_load->rep == XR_REP_TAGGED &&
            xr_aot_representation_materialization_verify(
                &view, fixture.function, fixture.target_plan, &policy,
                &diag));
    xr_aot_refinement_plan_free(plan);
    native_namespace_yieldable_storage_fixture_free(&fixture);

    NativeNamespaceYieldableStorageFixture extra =
        native_namespace_yieldable_storage_fixture_create(true);
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        extra.target_plan, &policy, &plan, &diag));
    REQUIRE(plan == NULL &&
            diag.issue ==
                XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE);
    native_namespace_yieldable_storage_fixture_free(&extra);
}

static void test_exact_string_literal_storage_is_tagged_and_fail_closed(void) {
    XiFunc *function = xi_func_new("representation_string_authority",
                                   &scalar_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *text = xi_const_str(function, entry, "not-a-machine-layout",
                                 &scalar_string);
    XiValue *print = xi_value_new(function, entry, XI_PRINT, &scalar_unit, 1);
    XiValue *release =
        xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(text != NULL && print != NULL && release != NULL);
    print->args[0] = text;
    release->args[0] = text;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(build_fixture_semantic_plan_and_attach(function, error,
                                              sizeof(error)));
    XrTargetProfile *profile = build_target_profile();
    XrTargetPlan *target_plan = NULL;
    REQUIRE(xr_target_plan_build(function->semantic_plan, profile,
                                 &target_plan, error, sizeof(error)));

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 0);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target_plan, function, text, &semantic_function, &semantic_value,
        error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan,
                                              binding->register_rep)
                : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan,
                                              binding->memory_rep)
                : NULL;
    REQUIRE(binding && register_rep && memory_rep &&
            register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            register_rep->ownership == XR_TARGET_OWNERSHIP_OWNED);
    xi_opt_refresh_representations_with_policy(function, &policy);
    REQUIRE(text->rep == XR_REP_TAGGED);
    bool materialized = xr_aot_representation_materialization_verify(
        &view, function, target_plan, &policy, &diag);
    if (!materialized)
        fprintf(stderr,
                "String literal materialization issue=%s record=%u value=%u operation=%u\n",
                xr_aot_refinement_issue_name(diag.issue), diag.record_index,
                diag.semantic_value, diag.semantic_operation);
    REQUIRE(materialized);

    xr_aot_refinement_plan_free(plan);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_stringbuilder_constructor_refinement_is_exact(void) {
    XrType stringbuilder_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 701,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .instance = {.class_name = "StringBuilder"},
    };
    XiFunc *function =
        xi_func_new("stringbuilder_refinement", &scalar_unit);
    XiBlock *entry = xi_block_new(function);
    XiValue *builder =
        xi_value_new(function, entry, XI_CALL_BUILTIN, &stringbuilder_type, 0);
    XiValue *release =
        xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(function && entry && builder && release);
    builder->aux = (void *) "StringBuilder";
    release->args[0] = builder;
    xi_block_set_return(entry, NULL);
    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified);
    xi_opt_refresh_representations_with_policy(function, &policy);
    REQUIRE(builder->rep == XR_REP_TAGGED);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));

    XrStableId saved_identity = target->calls[0].identity;
    target->calls[0].identity.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));
    target->calls[0].identity = saved_identity;
    uint8_t saved_convention = target->calls[0].calling_convention;
    target->calls[0].calling_convention = XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));
    target->calls[0].calling_convention = saved_convention;
    uint16_t saved_rep = target->calls[0].result_register_rep;
    target->calls[0].result_register_rep = target->calls[0].error_register_rep;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));
    target->calls[0].result_register_rep = saved_rep;
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));

    xr_aot_refinement_plan_free(plan);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_bundle_owns_empty_policy_bound_authority(void) {
    MaterializationFixture fixture = materialization_fixture_create();
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        fixture.target_plan, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.record_count == 0);
    xi_opt_refresh_representations_with_policy(fixture.function, &policy);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));

    XiModule module = {0};
    module.name = "representation_bundle";
    module.init = fixture.function;
    XiModule *modules[1] = {&module};
    XaotBundle bundle;
    REQUIRE(xaot_bundle_init(&bundle, modules, 1, 0));
    REQUIRE(xaot_bundle_set_target_plan(&bundle, 0,
                                        fixture.target_plan));
    REQUIRE(xaot_bundle_require_representation_refinements(&bundle));
    REQUIRE(xaot_bundle_install_representation_refinement(
        &bundle, 0, plan, &policy));
    REQUIRE(xaot_bundle_representation_refinement_for_module(&bundle, 0) ==
            plan);
    REQUIRE(xaot_bundle_representation_policy_matches(&bundle, 0,
                                                       &policy));
    XiRepPolicy stale_policy = policy;
    stale_policy.prefer_call_args_native = false;
    REQUIRE(!xaot_bundle_representation_policy_matches(
        &bundle, 0, &stale_policy));
    REQUIRE(!xaot_bundle_install_representation_refinement(
        &bundle, 0, plan, &stale_policy));

    xaot_bundle_free(&bundle);
    materialization_fixture_free(&fixture);
}

static void test_representation_adapters_are_immutable_and_consumable(void) {
    RepresentationFixture fixture = representation_fixture_create();
    uint32_t value_count = fixture.function->next_value_id;
    uint32_t block_value_count = fixture.entry->nvalues;
    XiValue *tagged_arg = fixture.tagged_call->args[0];
    XiValue *sum_arg = fixture.sum->args[0];
    uint8_t constant_rep = fixture.native_constant->rep;
    uint8_t call_rep = fixture.tagged_call->rep;
    XiValue **value_array = fixture.entry->values;
    XiValue *values[4] = {0};
    uint16_t nargs[4] = {0};
    XiValue *args[4][2] = {{0}};
    REQUIRE(block_value_count == 4);
    for (uint32_t i = 0; i < block_value_count; i++) {
        values[i] = fixture.entry->values[i];
        nargs[i] = values[i]->nargs;
        for (uint16_t a = 0; a < nargs[i] && a < 2; a++)
            args[i][a] = values[i]->args[a];
    }
    XrFingerprint semantic_before =
        xr_semantic_plan_fingerprint(fixture.function->semantic_plan);

    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 2);
    REQUIRE(view.records[0].transform_kind ==
            XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER);
    REQUIRE(view.records[0].decision == XR_AOT_REFINEMENT_APPLIED);
    REQUIRE(view.records[0].representation_adapter.adapter_kind ==
            XR_AOT_REP_ADAPTER_BOX);
    REQUIRE(view.records[0].representation_adapter.output_rep_kind ==
            XR_MACHINE_REP_DYN_VALUE);
    REQUIRE(view.records[1].representation_adapter.adapter_kind ==
            XR_AOT_REP_ADAPTER_UNBOX);
    REQUIRE(view.records[1].representation_adapter.input_rep_kind ==
            XR_MACHINE_REP_DYN_VALUE);

    REQUIRE(fixture.function->next_value_id == value_count);
    REQUIRE(fixture.entry->nvalues == block_value_count);
    REQUIRE(fixture.entry->values == value_array);
    for (uint32_t i = 0; i < block_value_count; i++) {
        REQUIRE(fixture.entry->values[i] == values[i]);
        REQUIRE(values[i]->nargs == nargs[i]);
        for (uint16_t a = 0; a < nargs[i] && a < 2; a++)
            REQUIRE(values[i]->args[a] == args[i][a]);
    }
    REQUIRE(fixture.tagged_call->args[0] == tagged_arg);
    REQUIRE(fixture.sum->args[0] == sum_arg);
    REQUIRE(fixture.native_constant->rep == constant_rep);
    REQUIRE(fixture.tagged_call->rep == call_rep);
    REQUIRE(xr_fingerprint_equal(
        semantic_before,
        xr_semantic_plan_fingerprint(fixture.function->semantic_plan)));

    xr_aot_refinement_plan_free(plan);
    representation_fixture_free(&fixture);
}

static void test_representation_record_mutations_fail_closed(void) {
    RepresentationFixture fixture = representation_fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView original = xr_aot_refinement_plan_view(plan);
    XrAotTransformationRecord records[2];
    memcpy(records, original.records, sizeof(records));
    XrAotRefinementPlanView mutated = original;
    mutated.records = records;

    records[0].input_state.generation[XR_AOT_INV_VALUES]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);

    memcpy(records, original.records, sizeof(records));
    records[0].protocol.pass_id++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    records[0].decision = XR_AOT_REFINEMENT_REFUSED;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.source_operation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.source_type_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.use_operation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.use_semantic_immediate++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.input_rep_kind = XR_MACHINE_REP_F64;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.recipe =
        XR_AOT_REP_RECIPE_UNBOX_INTEGER;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.machine_rep_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.reserved = 1;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    memcpy(records, original.records, sizeof(records));
    records[0].direct_call.target_call_index = 1;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.layout_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_LAYOUT);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    records[0].fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    mutated = original;
    mutated.fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    XrAotTransformationRecord swap = records[0];
    records[0] = records[1];
    records[1] = swap;
    mutated = original;
    mutated.records = records;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_NONCANONICAL_ORDER);

    records[0] = original.records[0];
    records[1] = original.records[0];
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_DUPLICATE_USE);

    mutated = original;
    mutated.record_count = XR_AOT_REFINEMENT_MAX_RECORDS + 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RESOURCE_BUDGET);

    xr_aot_refinement_plan_free(plan);
    representation_fixture_free(&fixture);
}

static XrTargetPlan *build_attached_target_plan(XiFunc *function,
                                                XrTargetProfile **out_profile) {
    char error[512] = {0};
    function->stage = XI_STAGE_OPTIMIZED;
    if (!build_fixture_semantic_plan_and_attach(function, error, sizeof(error)))
        fprintf(stderr, "semantic plan build for %s failed: %s\n", function->name, error);
    REQUIRE(function->semantic_plan != NULL);
    *out_profile = build_target_profile();
    XrTargetPlan *target_plan = NULL;
    if (!xr_target_plan_build(function->semantic_plan, *out_profile,
                              &target_plan, error, sizeof(error))) {
        fprintf(stderr, "target plan build for %s failed: %s\n",
                function->name, error);
        abort();
    }
    return target_plan;
}

static void test_borrowed_byte_slice_parameter_storage_is_exact_and_fail_closed(void) {
    XiFunc *function = xi_func_new("borrowed_byte_slice_parameter",
                                   &scalar_byte);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &borrowed_byte_slice);
    REQUIRE(parameter != NULL);
    function->nparams = 1;
    function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    function->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
        function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    XiValue *index = xi_const_int(function, entry, 0, &scalar_int);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET, &scalar_byte, 2);
    REQUIRE(index && read);
    read->args[0] = parameter;
    read->args[1] = index;
    xi_block_set_return(entry, read);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    char error[512] = {0};
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t parameter_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, parameter, &ignored_function, &parameter_value,
        error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target, parameter_value);
    REQUIRE(binding && binding->register_rep < target->machine_reps_count &&
            binding->memory_rep < target->machine_reps_count &&
            binding->slot < target->slots_count &&
            target->machine_reps[binding->register_rep].kind ==
                XR_MACHINE_REP_VIEW &&
            target->machine_reps[binding->memory_rep].kind ==
                XR_MACHINE_REP_VIEW);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan != NULL && xr_aot_refinement_plan_view(plan).record_count == 0);
    xr_aot_refinement_plan_free(plan);

    uint32_t saved_detail =
        target->machine_reps[binding->register_rep].detail;
    target->machine_reps[binding->register_rep].detail =
        XR_SEMANTIC_INDEX_NONE;
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->machine_reps[binding->register_rep].detail = saved_detail;

    uint8_t saved_root = target->slots[binding->slot].root_kind;
    target->slots[binding->slot].root_kind = XR_TARGET_ROOT_NONE;
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->slots[binding->slot].root_kind = saved_root;

    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_fixed_array_backing_projection_is_exact_and_fail_closed(void) {
    XiFunc *function = xi_func_new("fixed_array_backing_projection", &scalar_u32);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *array = xi_value_new(function, entry, XI_FIXED_ARRAY_NEW,
                                  &fixed_u32x4, 0);
    XiValue *index = xi_const_int(function, entry, 0, &scalar_int);
    XiValue *stored = xi_value_new(function, entry, XI_CONST, &scalar_u32, 0);
    XiValue *set = xi_value_new(function, entry, XI_INDEX_SET, &scalar_unit, 3);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET, &scalar_u32, 2);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(array && index && stored && set && read && release);
    array->aux_int = XR_NATIVE_U32;
    stored->aux_int = 7;
    set->args[0] = array;
    set->args[1] = index;
    set->args[2] = stored;
    read->args[0] = array;
    read->args[1] = index;
    release->args[0] = array;
    xi_block_set_return(entry, read);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, array, &semantic_function, &semantic_value, error,
        sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target, semantic_value);
    const XrTargetMachineRepRecord *register_rep = binding
        ? xr_target_plan_machine_rep(target, binding->register_rep)
        : NULL;
    const XrTargetMachineRepRecord *memory_rep = binding
        ? xr_target_plan_machine_rep(target, binding->memory_rep)
        : NULL;
    REQUIRE(binding && register_rep && memory_rep &&
            register_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            memory_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            register_rep->detail == memory_rep->detail);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    REQUIRE(refinement != NULL &&
            xr_aot_refinement_plan_view(refinement).record_count == 0);
    xr_aot_refinement_plan_free(refinement);

    XrCEmissionPlan *emission = NULL;
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, semantic_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_AGGREGATE &&
            view.address_projection ==
                XR_C_ADDRESS_PROJECTION_FIXED_ARRAY_BACKING &&
            view.backing_value == semantic_value &&
            view.backing_element_count == 4 &&
            view.backing_native_type == XR_NATIVE_U32 &&
            strcmp(view.c_type, "XrValue") == 0 &&
            strcmp(view.backing_c_type, "uint32_t") == 0);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == semantic_value) {
            REQUIRE(row == NULL);
            row = &emission->values[i];
        }
    REQUIRE(row != NULL);
    uint8_t saved_native = row->backing_native_type;
    row->backing_native_type = XR_NATIVE_U64;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->backing_native_type = saved_native;
    uint8_t saved_projection = row->address_projection;
    row->address_projection = XR_C_ADDRESS_PROJECTION_NAMED_AGGREGATE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->address_projection = saved_projection;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                      error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_named_aggregate_emission_is_exact_and_fail_closed(void) {
    const char *field_names[2] = {"low", "high"};
    XrType *field_types[2] = {&scalar_int, &scalar_int};
    XrAggregateLayout native_layout = {
        .field_count = 2,
        .kind = XR_AGG_LAYOUT_STRUCT,
        .explicit_align = 8,
        .nominal_name = "NativePair",
        .field_names = field_names,
    };
    XrClassInfo class_info = {
        .name = "NativePair",
        .struct_layout = &native_layout,
    };
    XrType named_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 101,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    named_type.instance.class_name = "NativePair";
    named_type.instance.class_ref = &class_info;
    XiClassData declaration = {
        .class_info = &class_info,
        .class_name = "NativePair",
        .instance_field_names = field_names,
        .instance_field_types = field_types,
        .instance_field_count = 2,
        .needs_runtime_type = false,
        .struct_layout = &native_layout,
    };
    XrType class_object = {
        .kind = XR_KIND_UNKNOWN,
        .id = 102,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };

    XiFunc *function = xi_func_new("named_aggregate_emission", &named_type);
    REQUIRE(function != NULL);
    function->is_module_initializer = true;
    XiModule *module = xi_module_new("named_aggregate_emission.xr",
                                     "named_aggregate_emission", function);
    REQUIRE(module != NULL);
    function->module = module;
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &declaration;
    module->nclasses = 1;
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *class_declaration =
        xi_value_new(function, entry, XI_CLASS_CREATE, &class_object, 0);
    XiValue *class_store =
        xi_value_new(function, entry, XI_SET_SHARED, &scalar_unit, 1);
    XiValue *class_load =
        xi_value_new(function, entry, XI_GET_SHARED, &class_object, 0);
    XiValue *aggregate =
        xi_value_new(function, entry, XI_AGG_NEW, &named_type, 1);
    REQUIRE(class_declaration && class_store && class_load && aggregate);
    class_declaration->aux = &declaration;
    class_store->args[0] = class_declaration;
    class_store->aux_int = 0;
    class_load->aux_int = 0;
    function->nshared = 1;
    aggregate->args[0] = class_load;
    aggregate->aux = &native_layout;
    xi_block_set_return(entry, aggregate);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, aggregate, &semantic_function, &semantic_value,
        error, sizeof(error)));
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, semantic_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_AGGREGATE &&
            view.target_register_kind == XR_MACHINE_REP_AGGREGATE &&
            view.target_memory_kind == XR_MACHINE_REP_AGGREGATE &&
            view.address_projection ==
                XR_C_ADDRESS_PROJECTION_NAMED_AGGREGATE &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            view.c_type && view.c_type[0]);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == semantic_value) {
            REQUIRE(row == NULL);
            row = &emission->values[i];
        }
    REQUIRE(row != NULL);
    uint8_t saved_rep = row->rep;
    row->rep = XR_C_VALUE_REP_TAGGED;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->rep = saved_rep;
    uint8_t saved_projection = row->address_projection;
    row->address_projection = XR_C_ADDRESS_PROJECTION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->address_projection = saved_projection;
    uint8_t saved_materialization = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_ARRAY_NEW;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->materialization = saved_materialization;
    const char *saved_c_type = row->c_type;
    row->c_type = "XrValue";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    row->c_type = saved_c_type;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                      error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
}

static void test_scalar_addressable_alias_recipe_is_exact_and_fail_closed(void) {
    XiFunc *function =
        xi_func_new("scalar_addressable_alias_recipe", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = 1;
    function->min_params = 1;
    function->params =
        (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    XiValue *source = xi_param(function, entry, 0, &scalar_int);
    XiValue *alias =
        xi_value_new(function, entry, XI_UNBOX, &scalar_int, 1);
    XiValue *place =
        xi_value_new(function, entry, XI_LOCAL_ADDR, &scalar_int, 1);
    XiValue *load =
        xi_value_new(function, entry, XI_PLACE_LOAD, &scalar_int, 1);
    REQUIRE(source && alias && place && load);
    function->params[0] = source;
    alias->args[0] = source;
    place->args[0] = alias;
    load->args[0] = place;
    xi_block_set_return(entry, load);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t alias_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, source, &semantic_function, &source_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, alias, &semantic_function, &alias_value, error,
        sizeof(error)));

    const XrTargetValueRepRecord *source_binding =
        xr_target_plan_value_rep(target, source_value);
    const XrTargetValueRepRecord *alias_binding =
        xr_target_plan_value_rep(target, alias_value);
    REQUIRE(source_binding && alias_binding &&
            source_binding->register_rep == alias_binding->register_rep &&
            source_binding->memory_rep == alias_binding->memory_rep);

    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, alias_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.materialization ==
                XR_C_VALUE_MATERIALIZATION_SCALAR_ADDRESSABLE_ALIAS &&
            view.recipe_operand_value == source_value &&
            view.recipe_argument_value == UINT32_MAX &&
            view.recipe_symbol == NULL &&
            view.rep == XR_C_VALUE_REP_I64);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == alias_value) {
            REQUIRE(row == NULL);
            row = &emission->values[i];
        }
    REQUIRE(row != NULL);
    uint32_t saved_source = row->recipe_operand_value;
    row->recipe_operand_value = alias_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_source;
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                      profile_fingerprint, error,
                                      sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_direct_local_tagged_ref_parameter_index_is_exact_and_fail_closed(void) {
    XiFunc *function = xi_func_new("direct_local_tagged_ref_parameter_index",
                                   &scalar_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *parameter =
        xi_param(function, entry, 0, &direct_local_byte_array);
    REQUIRE(parameter != NULL);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    REQUIRE(xi_func_set_param_passing_mode(function, 0, XR_PARAM_REF));
    parameter->transfer_mode = XR_TRANSFER_SHARE;
    function->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
        function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;

    XiValue *index = xi_const_int(function, entry, 0, &scalar_int);
    XiValue *element = xi_const_int(function, entry, 7, &scalar_byte);
    XiValue *write = xi_value_new(function, entry, XI_INDEX_SET,
                                  &scalar_unit, 3);
    REQUIRE(index && element && write);
    write->args[0] = parameter;
    write->args[1] = index;
    write->args[2] = element;
    xi_block_set_return(entry, NULL);
    XiModule *module = xi_module_new(
        "fixtures/direct_local_tagged_ref_parameter.xr",
        "direct_local_tagged_ref_parameter", function);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(
        module, "memory-module-v1:id=23:tagged-ref-parameter-v1"));
    function->module = module;

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    char error[512] = {0};
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t parameter_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, parameter, &ignored_function, &parameter_value,
        error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target, parameter_value);
    REQUIRE(binding && binding->slot < target->slots_count &&
            target->machine_reps[binding->register_rep].kind ==
                XR_MACHINE_REP_RAW_PTR &&
            target->machine_reps[binding->memory_rep].kind ==
                XR_MACHINE_REP_RAW_PTR &&
            target->slots[binding->slot].role == XR_TARGET_SLOT_PARAMETER &&
            target->slots[binding->slot].root_kind == XR_TARGET_ROOT_NONE &&
            target->slots[binding->slot].ownership ==
                XR_TARGET_OWNERSHIP_BORROWED);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan != NULL &&
            xr_aot_refinement_plan_view(plan).record_count == 0);
    xr_aot_refinement_plan_free(plan);

    uint8_t saved_ownership = target->slots[binding->slot].ownership;
    XrFingerprint saved_fingerprint = target->fingerprint;
    target->slots[binding->slot].ownership = XR_TARGET_OWNERSHIP_OWNED;
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->slots[binding->slot].ownership = saved_ownership;
    target->fingerprint = saved_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_enum_descriptor_adapter_refuses_without_layout_family(void) {
    RepresentationFixture fixture = representation_fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *native_plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView native_view =
        xr_aot_refinement_plan_view(native_plan);
    const XrAotRepresentationAdapterRecord *native =
        &native_view.records[0].representation_adapter;
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol =
        xr_aot_refinement_representation_protocol(27903);
    XrAotRepresentationAdapterRequest request = {
        .source_value = native->source_value,
        .use_operation = native->use_operation,
        .use_block = native->use_block,
        .use_operand = native->use_operand,
        .use_kind = native->use_kind,
        .adapter_kind = XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX,
        .input_rep_kind = native->input_rep_kind,
        .output_rep_kind = native->output_rep_kind,
        .layout = XR_SEMANTIC_INDEX_NONE,
        .policy_fingerprint = native->policy_fingerprint,
    };
    uint32_t decision = XR_AOT_REFINEMENT_APPLIED;
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        builder, &protocol, fixture.target_plan, &request, &decision, &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(diag.issue ==
            XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE);
    xr_aot_refinement_builder_free(builder);
    xr_aot_refinement_plan_free(native_plan);
    representation_fixture_free(&fixture);
}

static void test_tagged_string_array_copy_refinement_is_exact(void) {
    XiFunc *function = xi_func_new("tagged_string_array_copy_refinement", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_param(function, entry, 0, &tagged_string_array);
    XiValue *copy = xi_value_new(function, entry, XI_CALL_BUILTIN, &tagged_string_array, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    XiValue *result = xi_const_int(function, entry, 0, &scalar_int);
    REQUIRE(source && copy && release && result);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = source;
    function->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
        function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    copy->args[0] = source;
    copy->aux = (void *) "copy";
    copy->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED,
        .param_index = -1,
        .complete = true,
    };
    release->args[0] = copy;
    xi_block_set_return(entry, result);

    XiModule module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "tagged-string-array-copy-refinement.xr",
        .name = "tagged_string_array_copy_refinement",
        .init = function,
    };
    function->module = &module;

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t copy_value = XR_SEMANTIC_INDEX_NONE;
    char error[512] = {0};
    REQUIRE(xr_aot_scalar_semantic_value_id(target, function, copy, &semantic_function,
                                            &copy_value, error, sizeof(error)));
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != copy_value)
            continue;
        REQUIRE(operation == NULL);
        operation = candidate;
        operation_index = i;
    }
    REQUIRE(operation && operation_index != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(target, copy_value);
    REQUIRE(binding && binding->slot < target->slots_count &&
            target->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            target->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            target->slots[binding->slot].semantic_operation == operation_index &&
            target->slots[binding->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            target->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < target->layouts_count; i++) {
        if (target->layouts[i].semantic_type != operation->result_type)
            continue;
        REQUIRE(layout_index == XR_SEMANTIC_INDEX_NONE);
        layout_index = i;
    }
    REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE &&
            target->layouts[layout_index].kind == XR_TARGET_LAYOUT_DYNAMIC &&
            target->layouts[layout_index].array_element_storage ==
                XR_TARGET_ARRAY_STORAGE_TAGGED);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    XrAotRefinementPlanView refinement_view = xr_aot_refinement_plan_view(refinement);
    REQUIRE(refinement != NULL && refinement_view.record_count == 0);
    xi_opt_refresh_representations_with_policy(function, &policy);
    REQUIRE(xr_aot_representation_materialization_verify(
        &refinement_view, function, target, &policy, &diag));

    XrCEmissionPlan *emission = NULL;
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission, error,
                                     sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, copy_value, &view, error, sizeof(error)) &&
            view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            xr_c_emission_plan_verify(emission, target, profile_fingerprint, error,
                                      sizeof(error)));

    XrTargetLayoutRecord saved_layout = target->layouts[layout_index];
    XrFingerprint saved_fingerprint = target->fingerprint;
    target->layouts[layout_index].array_element_storage = XR_TARGET_ARRAY_STORAGE_U8;
    xr_target_layout_compute_fingerprint(target, layout_index,
                                         &target->layouts[layout_index].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)) &&
            strstr(error, "XR_TARGET_1002") != NULL);
    XrAotRefinementPlan *refused = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refused, &diag));
    REQUIRE(refused == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->layouts[layout_index] = saved_layout;
    target->fingerprint = saved_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_aot_refinement_plan_free(refinement);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    function->module = NULL;
    xi_func_free(function);
}

static void test_scalar_array_allocation_storage_is_exact_and_fail_closed(void) {
    XiFunc *function =
        xi_func_new("scalar_array_allocation_storage", &scalar_byte);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *count = xi_const_int(function, entry, 2, &scalar_int);
    XiValue *array = xi_value_new(function, entry, XI_ARRAY_NEW,
                                  &direct_local_byte_array, 1);
    XiValue *index = xi_const_int(function, entry, 0, &scalar_int);
    XiValue *element = xi_const_int(function, entry, 7, &scalar_byte);
    XiValue *write = xi_value_new(function, entry, XI_INDEX_SET,
                                  &scalar_unit, 3);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET,
                                 &scalar_byte, 2);
    REQUIRE(count && array && index && element && write && read);
    array->args[0] = count;
    array->array_element_storage = XR_ELEM_U8;
    write->args[0] = array;
    write->args[1] = index;
    write->args[2] = element;
    read->args[0] = array;
    read->args[1] = index;
    xi_block_set_return(entry, read);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t array_value = XR_SEMANTIC_INDEX_NONE;
    char error[512] = {0};
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, array, &semantic_function, &array_value,
        error, sizeof(error)));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target, array_value);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (candidate && candidate->result_value == array_value) {
            REQUIRE(operation == NULL);
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(binding && operation && operation->opcode == XI_ARRAY_NEW &&
            operation->array_element_storage == XR_ELEM_U8 &&
            operation->semantic_immediate == 0 &&
            binding->slot < target->slots_count &&
            target->slots[binding->slot].semantic_operation == operation_index &&
            target->slots[binding->slot].ownership ==
                XR_TARGET_OWNERSHIP_OWNED);
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < target->layouts_count; i++) {
        if (target->layouts[i].semantic_type != operation->result_type)
            continue;
        REQUIRE(layout_index == XR_SEMANTIC_INDEX_NONE);
        layout_index = i;
    }
    REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE &&
            target->layouts[layout_index].kind == XR_TARGET_LAYOUT_DYNAMIC &&
            target->layouts[layout_index].array_element_storage ==
                XR_TARGET_ARRAY_STORAGE_U8);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    REQUIRE(refinement != NULL &&
            xr_aot_refinement_plan_view(refinement).record_count == 0);
    xr_aot_refinement_plan_free(refinement);

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        target, xr_target_profile_fingerprint(profile), &emission,
        error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, array_value, &view, error, sizeof(error)) &&
            view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_NEW &&
            view.recipe_operand_value != UINT32_MAX &&
            view.recipe_argument_value == UINT32_MAX &&
            view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_U8 &&
            view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_array_new_typed") == 0);
    uint32_t emission_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == array_value) {
            REQUIRE(emission_index == XR_SEMANTIC_INDEX_NONE);
            emission_index = i;
        }
    REQUIRE(emission_index != XR_SEMANTIC_INDEX_NONE);
    emission->values[emission_index].recipe_discriminant =
        XR_TARGET_ARRAY_STORAGE_I64;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, xr_target_profile_fingerprint(profile),
        error, sizeof(error)));
    emission->values[emission_index].recipe_discriminant =
        XR_TARGET_ARRAY_STORAGE_U8;
    REQUIRE(xr_c_emission_plan_verify(
        emission, target, xr_target_profile_fingerprint(profile),
        error, sizeof(error)));
    xr_c_emission_plan_free(emission);

    XrTargetLayoutRecord saved_layout = target->layouts[layout_index];
    XrFingerprint saved_plan_fingerprint = target->fingerprint;
    target->layouts[layout_index].array_element_storage =
        XR_TARGET_ARRAY_STORAGE_I64;
    xr_target_layout_compute_fingerprint(
        target, layout_index, &target->layouts[layout_index].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)) &&
            strstr(error, "XR_TARGET_1002") != NULL);
    refinement = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    REQUIRE(refinement == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->layouts[layout_index] = saved_layout;
    target->fingerprint = saved_plan_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_array_intrinsic_index_storage_is_exact_and_fail_closed(void) {
    XiFunc *function = xi_func_new("array_intrinsic_index_storage", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *count = xi_const_int(function, entry, 3, &scalar_int);
    XiValue *fill = xi_const_int(function, entry, -1, &scalar_int);
    XiValue *array = xi_value_new(function, entry, XI_CALL_BUILTIN,
                                  &direct_local_int_array, 2);
    REQUIRE(count && fill && array);
    array->args[0] = count;
    array->args[1] = fill;
    array->aux = (void *) "array_filled_new";
    array->array_intrinsic_kind = XI_ARRAY_INTRINSIC_FILLED_NEW;
    array->array_element_storage = XR_ELEM_I64;

    XiValue *index = xi_const_int(function, entry, 1, &scalar_int);
    XiValue *element = xi_const_int(function, entry, 42, &scalar_int);
    XiValue *write = xi_value_new(function, entry, XI_INDEX_SET, &scalar_int, 3);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET, &scalar_int, 2);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(index && element && write && read && release);
    write->args[0] = array;
    write->args[1] = index;
    write->args[2] = element;
    read->args[0] = array;
    read->args[1] = index;
    release->args[0] = array;
    xi_block_set_return(entry, read);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    uint32_t array_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t read_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_CALL_BUILTIN &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW)
            array_operation = i;
        if (operation && operation->opcode == XI_INDEX_GET)
            read_value = operation->result_value;
    }
    REQUIRE(array_operation != XR_SEMANTIC_INDEX_NONE &&
            read_value != XR_SEMANTIC_INDEX_NONE);
    const XrSemanticOperationRecord *array_semantic =
        xr_semantic_plan_operation(semantic, array_operation);
    REQUIRE(array_semantic != NULL);
    const XrTargetValueRepRecord *array_binding =
        xr_target_plan_value_rep(target, array_semantic->result_value);
    const XrTargetValueRepRecord *read_binding =
        xr_target_plan_value_rep(target, read_value);
    REQUIRE(array_binding && read_binding &&
            target->machine_reps[array_binding->register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            target->machine_reps[array_binding->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_OWNED &&
            target->machine_reps[read_binding->register_rep].kind ==
                XR_MACHINE_REP_I64);

    uint32_t intrinsic_call_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < target->calls_count; i++)
        if (target->calls[i].semantic_operation == array_operation) {
            REQUIRE(intrinsic_call_index == XR_SEMANTIC_INDEX_NONE);
            intrinsic_call_index = i;
        }
    REQUIRE(intrinsic_call_index != XR_SEMANTIC_INDEX_NONE &&
            target->calls[intrinsic_call_index].calling_convention ==
                XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC &&
            target->calls[intrinsic_call_index].array_intrinsic_kind ==
                XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW &&
            target->calls[intrinsic_call_index].array_element_storage ==
                XR_TARGET_ARRAY_STORAGE_I64);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan != NULL && xr_aot_refinement_plan_view(plan).record_count == 0);
    xr_aot_refinement_plan_free(plan);

    char error[512] = {0};
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        target, xr_target_profile_fingerprint(profile), &emission, error,
        sizeof(error)));
    XrCValueEmissionView array_view = {0};
    XrCValueEmissionView read_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, array_semantic->result_value, &array_view, error,
                sizeof(error)) &&
            xr_c_emission_plan_value_view(
                emission, read_value, &read_view, error, sizeof(error)) &&
            array_view.rep == XR_C_VALUE_REP_TAGGED &&
            array_view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW &&
            array_view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_I64 &&
            read_view.rep == XR_C_VALUE_REP_I64 &&
            read_view.materialization == XR_C_VALUE_MATERIALIZATION_NONE);
    xr_c_emission_plan_free(emission);

    XrTargetCallRecord saved_call = target->calls[intrinsic_call_index];
    XrFingerprint saved_plan_fingerprint = target->fingerprint;
    target->calls[intrinsic_call_index].array_element_storage =
        XR_TARGET_ARRAY_STORAGE_U8;
    xr_target_call_compute_fingerprint(
        target, intrinsic_call_index,
        &target->calls[intrinsic_call_index].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)) &&
            strstr(error, "XR_TARGET_1003") != NULL);
    plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    REQUIRE(plan == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    target->calls[intrinsic_call_index] = saved_call;
    target->fingerprint = saved_plan_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_array_fill_scalar_authority_is_exact_and_fail_closed(void) {
    XiFunc *function = xi_func_new("array_fill_scalar_authority", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *count = xi_const_int(function, entry, 4, &scalar_int);
    XiValue *array = xi_value_new(function, entry, XI_ARRAY_NEW,
                                  &direct_local_int_array, 1);
    XiValue *fill = xi_const_int(function, entry, 6, &scalar_int);
    XiValue *filled = xi_value_new(function, entry, XI_CALL_METHOD,
                                   &direct_local_int_array, 2);
    XiValue *index = xi_const_int(function, entry, 0, &scalar_int);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET,
                                 &scalar_int, 2);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE,
                                    &scalar_unit, 1);
    REQUIRE(count && array && fill && filled && index && read && release);
    array->args[0] = count;
    array->array_element_storage = XR_ELEM_I64;
    filled->args[0] = array;
    filled->args[1] = fill;
    filled->aux = (void *) "fill";
    filled->aux_int = 2;
    filled->result_alias_operand = 0;
    filled->array_member_kind = XI_ARRAY_MEMBER_FILL;
    filled->array_element_storage = XR_ELEM_I64;
    read->args[0] = filled;
    read->args[1] = index;
    release->args[0] = filled;
    xi_block_set_return(entry, read);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    uint32_t fill_operation = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *fill_semantic = NULL;
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR) {
            REQUIRE(fill_semantic == NULL);
            fill_operation = i;
            fill_semantic = operation;
        }
    }
    REQUIRE(fill_semantic && fill_operation != XR_SEMANTIC_INDEX_NONE &&
            fill_semantic->opcode == XI_CALL_METHOD &&
            fill_semantic->operand_count == 2 &&
            fill_semantic->semantic_immediate == 0 &&
            fill_semantic->array_element_storage == XR_ELEM_I64 &&
            fill_semantic->result_alias_operand == 0);

    uint32_t call_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < target->calls_count; i++)
        if (target->calls[i].semantic_operation == fill_operation) {
            REQUIRE(call_index == XR_SEMANTIC_INDEX_NONE);
            call_index = i;
        }
    REQUIRE(call_index != XR_SEMANTIC_INDEX_NONE);
    XrTargetCallRecord *call = &target->calls[call_index];
    REQUIRE(call->calling_convention ==
                XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR &&
            call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR &&
            call->argument_count == 2 &&
            call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->argument_begin + 1u < target->call_arguments_count &&
            target->call_arguments[call->argument_begin].ordinal == 0 &&
            target->call_arguments[call->argument_begin].ownership ==
                XR_TARGET_CALL_BORROW &&
            target->call_arguments[call->argument_begin + 1u].ordinal == 1 &&
            target->call_arguments[call->argument_begin + 1u].ownership ==
                XR_TARGET_CALL_CONSUME);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    REQUIRE(refinement != NULL &&
            xr_aot_refinement_plan_view(refinement).record_count == 0);
    xr_aot_refinement_plan_free(refinement);

    char error[512] = {0};
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        target, xr_target_profile_fingerprint(profile), &emission, error,
        sizeof(error)));
    XrCValueEmissionView fill_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, fill_semantic->result_value, &fill_view, error,
                sizeof(error)) &&
            fill_view.rep == XR_C_VALUE_REP_TAGGED &&
            fill_view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR &&
            fill_view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_I64);
    uint32_t emission_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == fill_semantic->result_value) {
            REQUIRE(emission_index == XR_SEMANTIC_INDEX_NONE);
            emission_index = i;
        }
    REQUIRE(emission_index != XR_SEMANTIC_INDEX_NONE);
    uint32_t saved_recipe_argument =
        emission->values[emission_index].recipe_argument_value;
    emission->values[emission_index].recipe_argument_value =
        emission->values[emission_index].recipe_operand_value;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, xr_target_profile_fingerprint(profile), error,
        sizeof(error)));
    emission->values[emission_index].recipe_argument_value =
        saved_recipe_argument;
    emission->values[emission_index].recipe_discriminant =
        XR_TARGET_ARRAY_STORAGE_U8;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, xr_target_profile_fingerprint(profile), error,
        sizeof(error)));
    xr_c_emission_plan_free(emission);

    XrTargetCallRecord saved_call = *call;
    XrFingerprint saved_plan_fingerprint = target->fingerprint;
    call->target_kind = XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC;
    xr_target_call_compute_fingerprint(target, call_index,
                                       &call->fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)) &&
            strstr(error, "XR_TARGET_1003") != NULL);
    refinement = NULL;
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &diag));
    REQUIRE(refinement == NULL && diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
    *call = saved_call;
    target->fingerprint = saved_plan_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static XrTargetPlan *build_array_hof_refinement_fixture(
    uint8_t kind, XiFunc **out_function, XrTargetProfile **out_profile) {
    bool reduce = kind == XR_TARGET_ARRAY_HOF_REDUCE;
    bool filter = kind == XR_TARGET_ARRAY_HOF_FILTER;
    XrType *callback_type = reduce ? &array_hof_reduce_callback
                            : filter ? &array_hof_filter_callback
                                     : &array_hof_map_callback;
    XrType *callback_result = filter ? &scalar_bool : &scalar_int;
    XiFunc *root = xi_func_new(reduce ? "array_hof_reduce_refinement"
                                      : filter ? "array_hof_filter_refinement"
                                               : "array_hof_map_refinement",
                               &scalar_int);
    XiFunc *callback = xi_func_new("array_hof_refinement_callback",
                                   callback_result);
    REQUIRE(root && callback);
    XiBlock *entry = xi_block_new(root);
    XiBlock *callback_entry = xi_block_new(callback);
    REQUIRE(entry && callback_entry);
    callback->nparams = callback->min_params = reduce ? 2 : 1;
    callback->params = (XiValue **) xr_calloc(
        callback->nparams, sizeof(*callback->params));
    REQUIRE(callback->params != NULL);
    for (uint16_t i = 0; i < callback->nparams; i++) {
        callback->params[i] =
            xi_param(callback, callback_entry, i, &scalar_int);
        REQUIRE(callback->params[i] != NULL);
    }
    if (filter) {
        XiValue *accepted =
            xi_const_bool(callback, callback_entry, true, &scalar_bool);
        REQUIRE(accepted != NULL);
        xi_block_set_return(callback_entry, accepted);
    } else if (reduce) {
        XiValue *sum = xi_value_new(callback, callback_entry, XI_ADD,
                                    &scalar_int, 2);
        REQUIRE(sum != NULL);
        sum->args[0] = callback->params[0];
        sum->args[1] = callback->params[1];
        xi_block_set_return(callback_entry, sum);
    } else {
        xi_block_set_return(callback_entry, callback->params[0]);
    }
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = callback;
    root->nchildren = root->children_cap = 1;
    callback->parent_func = root;

    XiValue *capacity = xi_const_int(root, entry, 4, &scalar_int);
    XiValue *array = xi_value_new(root, entry, XI_ARRAY_NEW,
                                  &direct_local_int_array, 1);
    XiValue *closure =
        xi_value_new(root, entry, XI_CLOSURE_NEW, callback_type, 0);
    XiValue *initial =
        reduce ? xi_const_int(root, entry, 0, &scalar_int) : NULL;
    XiValue *hof = xi_value_new(root, entry, XI_CALL_METHOD,
                                reduce ? &scalar_int
                                       : &direct_local_int_array,
                                reduce ? 3 : 2);
    REQUIRE(capacity && array && closure && hof && (!reduce || initial));
    array->args[0] = capacity;
    array->array_element_storage = XR_ELEM_I64;
    closure->aux = callback;
    hof->args[0] = array;
    hof->args[1] = closure;
    if (reduce)
        hof->args[2] = initial;
    hof->aux = (void *) (reduce ? "reduce" : filter ? "filter" : "map");
    hof->array_hof_kind = reduce ? XI_ARRAY_HOF_REDUCE
                          : filter ? XI_ARRAY_HOF_FILTER
                                   : XI_ARRAY_HOF_MAP;
    hof->array_element_storage = XR_ELEM_I64;
    hof->array_result_element_storage = XR_ELEM_I64;
    XiValue *nonreduce_result = NULL;
    if (!reduce) {
        hof->call_return_ownership = (XiReturnOwnership) {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        XiValue *length =
            xi_value_new(root, entry, XI_LEN, &scalar_int, 1);
        XiValue *index = xi_const_int(root, entry, 0, &scalar_int);
        XiValue *element =
            xi_value_new(root, entry, XI_INDEX_GET, &scalar_int, 2);
        nonreduce_result =
            xi_value_new(root, entry, XI_ADD, &scalar_int, 2);
        XiValue *release_result =
            xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
        REQUIRE(length && index && element && nonreduce_result &&
                release_result);
        length->args[0] = hof;
        element->args[0] = hof;
        element->args[1] = index;
        nonreduce_result->args[0] = length;
        nonreduce_result->args[1] = element;
        release_result->args[0] = hof;
    }
    XiValue *release_array =
        xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release_array != NULL);
    release_array->args[0] = array;
    if (reduce) {
        xi_block_set_return(entry, hof);
    } else {
        xi_block_set_return(entry, nonreduce_result);
    }
    root->stage = callback->stage = XI_STAGE_OPTIMIZED;
    XrTargetPlan *target = build_attached_target_plan(root, out_profile);
    *out_function = root;
    return target;
}

static void expect_array_hof_mutation_refused(
    XrTargetPlan *target, const XiRepPolicy *policy) {
    XrAotRefinementPlan *refinement = NULL;
    XrAotRefinementDiagnostic diag = {0};
    REQUIRE(!xr_aot_representation_refinement_build_from_authority(
        target, policy, &refinement, &diag));
    REQUIRE(refinement == NULL &&
            diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);
}

static void test_array_hof_refinement_is_exact_and_fail_closed(void) {
    for (uint8_t kind = XR_TARGET_ARRAY_HOF_MAP;
         kind <= XR_TARGET_ARRAY_HOF_REDUCE; kind++) {
        XiFunc *function = NULL;
        XrTargetProfile *profile = NULL;
        XrTargetPlan *target = build_array_hof_refinement_fixture(
            kind, &function, &profile);
        const XrSemanticPlan *semantic =
            xr_target_plan_semantic_plan(target);
        uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation = NULL;
        for (uint32_t i = 0;
             i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
            const XrSemanticOperationRecord *candidate =
                xr_semantic_plan_operation(semantic, i);
            if (candidate && candidate->intrinsic_kind ==
                                 XR_SEM_INTRINSIC_ARRAY_HOF) {
                REQUIRE(operation == NULL);
                operation = candidate;
                operation_index = i;
            }
        }
        REQUIRE(operation && operation_index != XR_SEMANTIC_INDEX_NONE);
        uint32_t call_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; i < target->calls_count; i++) {
            if (target->calls[i].semantic_operation != operation_index)
                continue;
            REQUIRE(call_index == XR_SEMANTIC_INDEX_NONE);
            call_index = i;
        }
        REQUIRE(call_index != XR_SEMANTIC_INDEX_NONE);
        XrTargetCallRecord *call = &target->calls[call_index];
        REQUIRE(call->array_hof_kind == kind &&
                call->calling_convention ==
                    XR_TARGET_CALL_CONVENTION_ARRAY_HOF &&
                call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF &&
                call->argument_count ==
                    (kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3 : 2));
        XrTargetCallArgumentRecord *callback_argument =
            &target->call_arguments[call->argument_begin + 1u];
        const XrTargetValueRepRecord *callback_binding =
            xr_target_plan_value_rep(target,
                                     callback_argument->semantic_value);
        const XrTargetValueRepRecord *result_binding =
            xr_target_plan_value_rep(target, operation->result_value);
        REQUIRE(callback_binding && result_binding &&
                target->machine_reps[callback_binding->register_rep].kind ==
                    XR_MACHINE_REP_DYN_VALUE &&
                target->slots[callback_binding->slot].root_kind ==
                    XR_TARGET_ROOT_DYNAMIC &&
                target->slots[callback_binding->slot].ownership ==
                    XR_TARGET_OWNERSHIP_OWNED);
        if (kind == XR_TARGET_ARRAY_HOF_REDUCE) {
            REQUIRE(target->machine_reps[result_binding->register_rep].kind ==
                        XR_MACHINE_REP_I64 &&
                    target->slots[result_binding->slot].root_kind ==
                        XR_TARGET_ROOT_NONE &&
                    target->slots[result_binding->slot].ownership ==
                        XR_TARGET_OWNERSHIP_TRIVIAL);
        } else {
            REQUIRE(target->machine_reps[result_binding->register_rep].kind ==
                        XR_MACHINE_REP_DYN_VALUE &&
                    target->slots[result_binding->slot].root_kind ==
                        XR_TARGET_ROOT_DYNAMIC &&
                    target->slots[result_binding->slot].ownership ==
                        XR_TARGET_OWNERSHIP_OWNED);
        }
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        XrAotRefinementDiagnostic diag = {0};
        XrAotRefinementPlan *refinement = NULL;
        REQUIRE(xr_aot_representation_refinement_build_from_authority(
            target, &policy, &refinement, &diag));
        REQUIRE(refinement &&
                xr_aot_refinement_plan_view(refinement).record_count == 0);
        xr_aot_refinement_plan_free(refinement);

        if (kind == XR_TARGET_ARRAY_HOF_MAP) {
            char error[512] = {0};
            XrFingerprint saved_plan_fingerprint = target->fingerprint;
            XrTargetCallRecord saved_call = *call;
            call->callee_function = XR_SEMANTIC_INDEX_NONE;
            xr_target_call_compute_fingerprint(target, call_index,
                                               &call->fingerprint);
            xr_target_plan_compute_fingerprint(target, &target->fingerprint);
            expect_array_hof_mutation_refused(target, &policy);
            *call = saved_call;
            target->fingerprint = saved_plan_fingerprint;

            XrTargetCallArgumentRecord saved_argument = *callback_argument;
            callback_argument->semantic_value =
                target->call_arguments[call->argument_begin].semantic_value;
            xr_target_call_compute_fingerprint(target, call_index,
                                               &call->fingerprint);
            xr_target_plan_compute_fingerprint(target, &target->fingerprint);
            expect_array_hof_mutation_refused(target, &policy);
            *callback_argument = saved_argument;
            call->fingerprint = saved_call.fingerprint;
            target->fingerprint = saved_plan_fingerprint;

            XrTargetSlotRecord saved_slot =
                target->slots[result_binding->slot];
            target->slots[result_binding->slot].root_kind =
                XR_TARGET_ROOT_NONE;
            xr_target_plan_compute_fingerprint(target, &target->fingerprint);
            expect_array_hof_mutation_refused(target, &policy);
            target->slots[result_binding->slot] = saved_slot;
            target->fingerprint = saved_plan_fingerprint;

            uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
            for (uint32_t i = 0; i < target->layouts_count; i++)
                if (target->layouts[i].semantic_type ==
                    operation->result_type)
                    layout_index = i;
            REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE);
            XrTargetLayoutRecord saved_layout =
                target->layouts[layout_index];
            target->layouts[layout_index].array_element_storage =
                XR_TARGET_ARRAY_STORAGE_U8;
            xr_target_layout_compute_fingerprint(
                target, layout_index,
                &target->layouts[layout_index].fingerprint);
            xr_target_plan_compute_fingerprint(target, &target->fingerprint);
            expect_array_hof_mutation_refused(target, &policy);
            target->layouts[layout_index] = saved_layout;
            target->fingerprint = saved_plan_fingerprint;
            REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));
        }
        xr_target_plan_free(target);
        xr_target_profile_free(profile);
        xi_func_free(function);
    }
}

static void test_string_concat_cleanup_materialization_is_exact(void) {
    XiFunc *function =
        xi_func_new("string_concat_cleanup_materialization", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_str(function, entry, "left", &scalar_string);
    XiValue *right = xi_const_str(function, entry, "right", &scalar_string);
    XiValue *concat =
        xi_value_new(function, entry, XI_STR_CONCAT, &scalar_string, 2);
    XiValue *release =
        xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    XiValue *result = xi_const_int(function, entry, 0, &scalar_int);
    REQUIRE(left && right && concat && release && result);
    concat->args[0] = left;
    concat->args[1] = right;
    release->args[0] = concat;
    xi_block_set_return(entry, result);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_attached_target_plan(function, &profile);
    REQUIRE(target->cleanups_count == 1 &&
            target->cleanups[0].action == XR_TARGET_CLEANUP_RELEASE);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    xi_opt_refresh_representations_with_policy(function, &policy);
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));

    XiValue *saved_argument = release->args[0];
    release->args[0] = release;
    REQUIRE(!xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    release->args[0] = saved_argument;
    REQUIRE(xr_aot_representation_materialization_verify(
        &view, function, target, &policy, &diag));

    xr_aot_refinement_plan_free(plan);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "tagged-string-array-copy-refinement") == 0) {
        test_tagged_string_array_copy_refinement_is_exact();
        puts("Tagged String Array copy AOT refinement tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "tagged-ref") == 0) {
        test_direct_local_tagged_ref_argument_authority_is_exact();
        test_direct_local_tagged_ref_parameter_index_is_exact_and_fail_closed();
        printf("TargetPlan tagged-ref AOT refinement tests passed\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "unknown focused test selector\n");
        return 2;
    }
    test_open_target_direct_call_refuses_without_baseline_change();
    test_direct_call_authority_applies_closed_local_binding();
    test_tail_call_backend_conformance_is_exact();
    test_direct_call_binding_mutations_fail_closed();
    test_direct_call_out_of_range_request_fails_closed();
    test_stale_state_and_baseline_mutations_fail_closed();
    test_representation_adapters_are_immutable_and_consumable();
    test_immutable_authority_matches_backend_materialization();
    test_scalar_shared_boundary_is_exact_and_fail_closed();
    test_exact_heap_closure_storage_is_tagged_and_fail_closed();
    test_direct_local_shared_callee_storage_is_exact_and_fail_closed();
    test_direct_local_tagged_ref_argument_authority_is_exact();
    test_direct_local_go_callee_storage_is_exact_and_fail_closed();
    test_source_namespace_storage_is_exact_and_fail_closed();
    test_standalone_source_namespace_storage_is_exact_and_fail_closed();
    test_native_namespace_yieldable_storage_uses_frozen_call_identity();
    test_exact_string_literal_storage_is_tagged_and_fail_closed();
    test_stringbuilder_constructor_refinement_is_exact();
    test_bundle_owns_empty_policy_bound_authority();
    test_representation_record_mutations_fail_closed();
    test_borrowed_byte_slice_parameter_storage_is_exact_and_fail_closed();
    test_fixed_array_backing_projection_is_exact_and_fail_closed();
    test_named_aggregate_emission_is_exact_and_fail_closed();
    test_scalar_addressable_alias_recipe_is_exact_and_fail_closed();
    test_direct_local_tagged_ref_parameter_index_is_exact_and_fail_closed();
    test_tagged_string_array_copy_refinement_is_exact();
    test_scalar_array_allocation_storage_is_exact_and_fail_closed();
    test_array_intrinsic_index_storage_is_exact_and_fail_closed();
    test_array_fill_scalar_authority_is_exact_and_fail_closed();
    test_array_hof_refinement_is_exact_and_fail_closed();
    test_string_concat_cleanup_materialization_is_exact();
    test_enum_descriptor_adapter_refuses_without_layout_family();
    printf("TargetPlan-native AOT refinement tests passed\n");
    return 0;
}
