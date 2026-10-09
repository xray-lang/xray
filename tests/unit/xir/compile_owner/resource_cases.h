/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * resource_cases.h - Independent work arithmetic and metadata owner failure gates
 */
#include "../xir_construction_fixture.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_implementation_verify.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_interface_members.h"
#include "xir/xxir_defaults_internal.h"
#include "xir/xxir_storage.h"
#include "xir/xxir_type_inference.h"

static const XrXirType copy_arguments[] = {XR_XIR_I64, XR_XIR_STRING};
static const XrXirInterfaceApplication copy_application = {0,copy_arguments,2};
static const XrXirConstraint copy_constraint = {0,&copy_application,1};

static void constraint_formula(void) {
    /* Two table visits, three allocation attempts, two clears and one array copy. */
    const uint64_t work = 2 + 3 + sizeof(XrXirConstraint) + sizeof(XrXirInterfaceApplication) + sizeof(copy_arguments);
    for (unsigned below = 0; below < 2; ++below) {
        reset(SIZE_MAX);
        XrCompileResourceLimits limits = unlimited; limits.work = 1 + work - below;
        XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
        XrXirConstraint sentinel = {0}, *copy = &sentinel;
        XrXirStatus result = xr_xir_compile_constraint_array_copy_verified(&context,&copy_constraint,1,&copy);
        CHECK(result == (below ? XR_XIR_BUDGET : XR_XIR_OK));
        if (below) CHECK(copy == &sentinel);
        else {
            CHECK(attempts == 4 && stats(owner).work == 1 + work);
            CHECK(copy != &copy_constraint && copy->interfaces != &copy_application);
            CHECK(copy->interfaces[0].arguments != copy_arguments);
            CHECK(copy->interfaces[0].arguments[1] == XR_XIR_STRING);
            xr_xir_compile_constraint_array_free(copy,1);
            CHECK(live_count == 1);
            copy = &sentinel;
            CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&copy_constraint,1,&copy) == XR_XIR_BUDGET);
            CHECK(copy == &sentinel && stats(owner).work == 1 + work);
        }
        CHECK(live_count == 1);
        xr_compile_resources_release(owner);
    }
    for (size_t failure = 1; failure <= 3; ++failure) {
        reset(failure);
        XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
        XrXirConstraint sentinel = {0}, *copy = &sentinel;
        CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&copy_constraint,1,&copy) == XR_XIR_OUT_OF_MEMORY);
        uint64_t spent = failure == 1 ? 3 : failure == 2 ? 5 + sizeof(XrXirConstraint) :
            6 + sizeof(XrXirConstraint) + sizeof(XrXirInterfaceApplication);
        CHECK(copy == &sentinel && stats(owner).work == spent && live_count == 1);
        xr_compile_resources_release(owner);
    }
    puts("constraint formula: fixed work exact/minus1, retained failed work and cumulative repeat refusal");
}

static const XrXirConstraint empty_constraints[2] = {{0},{0}};
static const XrXirConstraint copied_constraints[2] = {{0,&copy_application,1},{0}};
static const XrXirCallableParameter copied_parameter = {XR_XIR_I64,0};
static const XrXirTypeNode copied_signature = {
    .kind=XR_XIR_TYPE_CALLABLE,.parameters=&copied_parameter,.parameter_count=1,.result=XR_XIR_I64,
    .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED
};
static const XrXirInterfaceMethod copied_method = {{"run",3},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,1,&copy_constraint};
static const XrXirInterfaceDeclaration copied_declarations[] = {
    {{"api",3},{"Base",4},1,empty_constraints,2,NULL,0,NULL,0},
    {{"api",3},{"Printable",9},1,copied_constraints,2,&copy_application,1,&copied_method,1}
};
static const XrXirInterfaceTable copied_interfaces = {copied_declarations,2};
static const XrXirTypes copied_types = {&copied_signature,1,NULL,&copied_interfaces};

static XrXirStatus metadata_copy(const XrXirCompileContext *context, unsigned kind, bool release_caller) {
    XrXirStatus result;
    if (kind == 2) {
        XrXirInterfaceClosure *sentinel = (XrXirInterfaceClosure *)context, *copy = sentinel;
        XrXirInterfaceClosureRoots request = {&copied_interfaces,&copied_types,&copy_application,1,0};
        result = xr_xir_compile_types_structure_verify(context,&copied_types);
        if (result == XR_XIR_OK) result = xr_xir_compile_interface_closure_build(context,&request,&copy);
        if (result != XR_XIR_OK) CHECK(copy == sentinel);
        else {
            if (release_caller) xr_compile_resources_release(context->resources);
            CHECK(xr_xir_interface_closure_application_count(copy) == 1);
            xr_xir_compile_interface_closure_free(copy);
        }
    } else if (kind == 0) {
        XrXirInterfaceTable sentinel = {0}, *copy = &sentinel;
        result = xr_xir_compile_types_structure_verify(context,&copied_types);
        if (result == XR_XIR_OK) result = xr_xir_compile_interfaces_copy_verified(context,&copied_interfaces,&copy);
        if (result != XR_XIR_OK) CHECK(copy == &sentinel);
        else {
            CHECK(copy->declarations[1].methods != &copied_method);
            CHECK(copy->declarations[1].methods[0].constraints->interfaces[0].arguments != copy_arguments);
            if (release_caller) xr_compile_resources_release(context->resources);
            CHECK(!memcmp(copy->declarations[1].name.bytes,"Printable",9));
            xr_xir_compile_interfaces_free(copy);
        }
    } else {
        XrXirImplementationTable sentinel = {0}, *copy = &sentinel;
        XrXirArtifact *producer = NULL;
        result = implementation_built(context,&producer);
        if (result == XR_XIR_OK) result = xr_xir_compile_implementations_copy_verified(context,
            xr_xir_compile_artifact_module(producer)->declarations->implementations,&copy);
        xr_xir_compile_artifact_free(producer);
        if (result != XR_XIR_OK) CHECK(copy == &sentinel);
        else {
            CHECK(copy->records[0].bindings[0].function == 2);
            if (release_caller) xr_compile_resources_release(context->resources);
            CHECK(copy->records[0].bindings[0].requirement.arguments[0] == XR_XIR_I64);
            xr_xir_compile_implementations_free(copy);
        }
    }
    return result;
}
static void metadata_failures(unsigned kind) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    CHECK(metadata_copy(&context,kind,false) == XR_XIR_OK);
    size_t count = attempts;
    uint64_t work = stats(owner).work;
    xr_compile_resources_release(owner);
    for (size_t failure = 1; failure < count; ++failure) {
        reset(failure); owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK); context.resources = owner;
        CHECK(metadata_copy(&context,kind,false) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live_count == 1); xr_compile_resources_release(owner);
    }
    for (uint64_t cap = 1; cap < work; cap += work / 128 + 1) {
        reset(SIZE_MAX); owner = NULL;
        XrCompileResourceLimits limits = unlimited; limits.work = cap;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK); context.resources = owner;
        CHECK(metadata_copy(&context,kind,false) == XR_XIR_BUDGET);
        CHECK(live_count == 1); xr_compile_resources_release(owner);
    }
    reset(SIZE_MAX); owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK); context.resources = owner;
    CHECK(metadata_copy(&context,kind,true) == XR_XIR_OK);
    CHECK(!live && !live_count);
    printf("metadata %u allocations=%zu; all OOM and distributed work boundaries; physical zero\n",kind,count);
}

static void mandatory_context(void) {
    XrXirCompileContext missing = {0};
    XrXirArtifact *artifact = (XrXirArtifact *)&missing;
    CHECK(xir_fixture_check(&missing, &module, &artifact, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(artifact == (XrXirArtifact *)&missing);
    CHECK(xr_xir_compile_checked_read(NULL,NULL,0,&artifact,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(artifact == (XrXirArtifact *)&missing);
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirWitness witness = {99,copy_arguments,2};
    CHECK(xr_xir_compile_witness_resolve(&context,NULL,NULL,&witness) == XR_XIR_BAD_STRUCTURE);
    CHECK(witness.function == 99 && witness.arguments == copy_arguments && witness.argument_count == 2);
    XrXirTypes sentinel = {0}, *types = &sentinel;
    CHECK(xr_xir_compile_types_clone(&context,NULL,&types) == XR_XIR_OK && !types);
    XrXirConstraint constraint = {0}, *copy = &constraint;
    CHECK(xr_xir_compile_constraint_array_copy_verified(&context,NULL,1,&copy) == XR_XIR_BAD_STRUCTURE);
    CHECK(copy == &constraint);
    CHECK(xr_xir_compile_constraint_array_copy_verified(&context,NULL,0,&copy) == XR_XIR_OK && !copy);
    XrXirLayout layout = {99,77};
    CHECK(xr_xir_compile_layout(&context,NULL,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,&target,XR_XIR_LAYOUT_STORAGE,&layout) == XR_XIR_BAD_LAYOUT);
    CHECK(layout.size == 99 && layout.alignment == 77);
    CHECK(live_count == 1); xr_compile_resources_release(owner);
}

static void exhausted_owner(void) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(&context, &module, &checked, NULL) == XR_XIR_OK);
    XrCompileResourceStats before = stats(owner);
    CHECK(xr_compile_resources_work(owner,UINT64_MAX - before.work) == XR_COMPILE_RESOURCE_OK);
    XrXirDefaultBinding binding = {0};
    XrXirDefaultTable defaults = {&binding,1};
    XrXirModule with_defaults = module; with_defaults.defaults = &defaults;
    const XrXirDefaultBinding *lookup = &binding;
    bool helper = true;
    CHECK(xr_xir_compile_default_lookup(&context,&with_defaults,0,0,&lookup) == XR_XIR_BUDGET && lookup == &binding);
    CHECK(xr_xir_compile_default_helper(&context,&with_defaults,0,&helper) == XR_XIR_BUDGET && helper);
    XrXirArtifact *out = NULL;
    CHECK(xr_xir_compile_lower(checked,&target,&out,NULL) == XR_XIR_BUDGET && !out);
    CHECK(xr_xir_compile_specialize(checked,&out,NULL) == XR_XIR_BUDGET && !out);
    CHECK(xir_fixture_check(&context, &module, &out, NULL) == XR_XIR_BUDGET && !out);
    CHECK(xr_xir_compile_recheck_v2(&context, xr_xir_compile_artifact_module(checked), xr_xir_compile_artifact_construction(checked), &out, NULL) == XR_XIR_BUDGET && !out);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL) == XR_XIR_BUDGET);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_BUDGET);
    CHECK(!packet.bytes && !packet.length);
    CHECK(stats(owner).live_bytes == before.live_bytes);
    xr_compile_resources_release(owner);
    xr_xir_compile_artifact_free(checked);
    CHECK(!live && !live_count);
}

static void structural_caps(void) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    /* One callable parameter, four declaration parameters and one method parameter. */
    context.limits.parameters = 6;
    CHECK(xr_xir_compile_types_structure_verify(&context,&copied_types) == XR_XIR_OK);
    context.limits.parameters = 5;
    CHECK(xr_xir_compile_types_structure_verify(&context,&copied_types) == XR_XIR_BUDGET);
    /* One generic parameter plus the two caller and one callee value parameters. */
    context.limits.parameters = 4;
    XrXirArtifact *artifact = NULL;
    CHECK(generic_built(&context,&artifact) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact); artifact = NULL;
    context.limits.parameters = 3;
    CHECK(generic_built(&context,&artifact) == XR_XIR_BUDGET && !artifact);
    CHECK(live_count == 1); xr_compile_resources_release(owner);
}

static XrXirStatus inference_owner_begin(XrCompileResources *owner, XrXirInferenceState **output) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirType prefix = XR_XIR_I64;
    const uint32_t kinds[] = {0,XR_XIR_BINDER_RESULT_VARIABLE};
    XrXirInferenceRequest request = {NULL,&prefix,1,1,0,kinds};
    return xr_xir_compile_inference_begin(&context,&request,output);
}
static XrXirStatus inference_owner_run(XrCompileResources *owner, bool release_caller) {
    XrXirInferenceState *state = NULL;
    XrXirStatus status = inference_owner_begin(owner,&state);
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_observe(state,NULL,
        (XrXirInferencePair){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),XR_XIR_UNIT});
    XrXirInferenceKnown known = {true,copy_arguments,17};
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_expected_known(state,NULL,
        (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),&known);
    if (status == XR_XIR_OK) CHECK(known.known && known.argument_count == 2);
    else CHECK(known.known && known.arguments == copy_arguments && known.argument_count == 17);
    XrXirType output[] = {XR_XIR_STRING,XR_XIR_STRING};
    if (status == XR_XIR_OK) {
        if (release_caller) xr_compile_resources_release(owner);
        status = xr_xir_compile_inference_finalize(state,NULL,output,2);
    }
    if (status == XR_XIR_OK) CHECK(output[0] == XR_XIR_I64 && output[1] == XR_XIR_UNIT);
    else CHECK(output[0] == XR_XIR_STRING && output[1] == XR_XIR_STRING);
    xr_xir_compile_inference_dispose(state);
    return status;
}
static void inference_failures(void) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(inference_owner_run(owner,false) == XR_XIR_OK);
    size_t count = attempts; uint64_t work = stats(owner).work;
    xr_compile_resources_release(owner);
    for (size_t failure = 1; failure < count; ++failure) {
        reset(failure); owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(inference_owner_run(owner,false) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live_count == 1); xr_compile_resources_release(owner);
    }
    for (uint64_t cap = 1; cap < work; ++cap) {
        reset(SIZE_MAX); owner = NULL;
        XrCompileResourceLimits limits = unlimited; limits.work = cap;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(inference_owner_run(owner,false) == XR_XIR_BUDGET);
        CHECK(live_count == 1); xr_compile_resources_release(owner);
    }
    reset(SIZE_MAX); owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(inference_owner_run(owner,true) == XR_XIR_OK && !live_count && !live);
    printf("inference allocations=%zu; all OOM and work boundaries; producer context lifetime\n",count);
}
