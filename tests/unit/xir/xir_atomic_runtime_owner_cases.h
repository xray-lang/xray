/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_runtime_owner_cases.h - Failure barriers and real owner domains
 */
#ifndef XIR_ATOMIC_RUNTIME_OWNER_CASES_H
#define XIR_ATOMIC_RUNTIME_OWNER_CASES_H
static void core_runtime_fault_case(unsigned mode, size_t ordinal, size_t *sites) {
    CoreOwner owner = {0}; core_owner_new(&owner);
    XrXirValue initial = core_scalar(XR_XIR_I64, 7), receiver = {0}, output = {0};
    XrXirValue operands[2] = {initial, core_scalar(XR_XIR_I64, 9)};
    if (mode) CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
    size_t live = runtime_observer.live, bytes = runtime_observer.bytes;
    size_t begin = runtime_observer.attempts;
    if (ordinal != SIZE_MAX) runtime_observer.fail_at = begin + ordinal;
    XrXirValueStatus value_status = XR_XIR_VALUE_OK;
    XrXirRunStatus run_status = XR_XIR_RUN_OK;
    if (!mode) value_status = xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &output);
    else run_status = core_execute(&owner, &receiver, mode == 1 ? XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE :
        XR_XIR_ATOMIC_OPERATION_TO_STRING, operands, NULL, &output).status;
    size_t completed = runtime_observer.attempts - begin;
    runtime_observer.fail_at = SIZE_MAX;
    if (ordinal == SIZE_MAX) {
        CHECK(value_status == XR_XIR_VALUE_OK && run_status == XR_XIR_RUN_OK);
        *sites = completed;
    } else {
        CHECK(completed > ordinal);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(mode ? run_status == XR_XIR_RUN_OUT_OF_MEMORY : value_status == XR_XIR_VALUE_OOM);
        CHECK(runtime_observer.live == live && runtime_observer.bytes == bytes);
        if (mode) CHECK(core_load(&owner, &receiver) == 7);
    }
    xr_xir_value_drop(&output); xr_xir_value_drop(&receiver); core_owner_free(&owner);
}
static void core_runtime_faults(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        size_t sites = 0; core_runtime_fault_case(mode, SIZE_MAX, &sites);
        CHECK(sites && sites < 128);
        for (size_t site = 0; site < sites; ++site) core_runtime_fault_case(mode, site, &sites);
        printf("atomic runtime prepare mode=%u actual_faults=%zu physical=0\n", mode, sites);
    }
    CoreOwner owner = {0}; core_owner_new(&owner);
    XrXirValue initial = core_scalar(XR_XIR_I64, 7), operands[2] = {initial, core_scalar(XR_XIR_I64, 9)};
    XrXirValue receiver = {0}, output = {0};
    CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
    uint64_t begin = owner.admission.work;
    CHECK(core_execute(&owner, &receiver, XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, operands, NULL, &output).status == XR_XIR_RUN_OK);
    uint64_t required = begin - owner.admission.work; CHECK(required && required < 128);
    xr_xir_value_drop(&output); xr_xir_value_drop(&receiver);
    for (uint64_t cut = 0; cut <= required; ++cut) {
        owner.admission.work = 1048576;
        CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
        owner.admission.work = cut;
        XrXirRunStatus status = core_execute(&owner, &receiver, XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, operands, NULL, &output).status;
        owner.admission.work = 1048576;
        CHECK(status == (cut == required ? XR_XIR_RUN_OK : XR_XIR_RUN_STEP_LIMIT));
        CHECK(core_load(&owner, &receiver) == (cut == required ? 9u : 7u));
        if (cut != required) CHECK(!output.type && !output.payload);
        xr_xir_value_drop(&output); xr_xir_value_drop(&receiver);
    }
    XirAtomic *cell = NULL;
    CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
    cell = (XirAtomic *)object_pointer(&receiver);
    atomic_store_explicit(&cell->object.references, INT32_MAX, memory_order_relaxed);
    CHECK(core_execute(&owner, &receiver, XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, operands, NULL, &output).status == XR_XIR_RUN_STEP_LIMIT);
    atomic_store_explicit(&cell->object.references, 1, memory_order_relaxed);
    CHECK(core_load(&owner, &receiver) == 7 && !output.type);
    xr_xir_value_drop(&receiver); core_owner_free(&owner);
    printf("atomic CAS all_work_prefixes=%" PRIu64 " ref_saturation physical=0\n", required + 1);
}
static XrXirValueStatus core_arena_probe(const XrCompileResourceLimits *limits, size_t ordinal,
    XrCompileResourceStats *stats, size_t *attempts) {
    size_t begin = compiler_observer.attempts;
    if (ordinal != SIZE_MAX) compiler_observer.fail_at = begin + ordinal;
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrXirTypeArena *arena = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &context.resources);
    XrXirValueStatus result = status == XR_COMPILE_RESOURCE_OK ? core_arena(&context, &arena) :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;
    if (context.resources && stats) CHECK(xr_compile_resources_stats(context.resources, stats) == XR_COMPILE_RESOURCE_OK);
    *attempts = compiler_observer.attempts - begin;
    compiler_observer.fail_at = SIZE_MAX;
    if (result != XR_XIR_VALUE_OK) CHECK(!arena);
    xr_xir_compile_type_arena_drop(arena); xr_compile_resources_release(context.resources);
    CHECK(!compiler_observer.live && !compiler_observer.bytes && !runtime_observer.live && !runtime_observer.bytes);
    return result;
}
static void core_compiler_faults(void) {
    XrCompileResourceLimits limits = {UINT64_C(32) << 20, UINT64_C(8) << 20, UINT64_C(64) << 20};
    XrCompileResourceStats baseline = {0}; size_t sites = 0, attempts = 0;
    CHECK(core_arena_probe(&limits, SIZE_MAX, &baseline, &sites) == XR_XIR_VALUE_OK && sites && sites < 20000);
    for (size_t i = 0; i < sites; ++i) {
        CHECK(core_arena_probe(&limits, i, NULL, &attempts) == XR_XIR_VALUE_OOM && attempts > i);
    }
    limits = (XrCompileResourceLimits){baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    CHECK(core_arena_probe(&limits, SIZE_MAX, NULL, &attempts) == XR_XIR_VALUE_OK);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits cut = limits;
        if (!axis) --cut.allocated_bytes; else if (axis == 1) --cut.live_bytes; else --cut.work;
        CHECK(core_arena_probe(&cut, SIZE_MAX, NULL, &attempts) == XR_XIR_VALUE_LIMIT);
    }
    printf("atomic arena compiler_faults=%zu allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " exact-minus1=3 physical=0\n",
        sites, baseline.allocated_bytes, baseline.peak_bytes, baseline.work);
}
static void core_carriers_and_capability(void) {
    CoreOwner owner = {0}; core_owner_new(&owner);
    size_t live = runtime_observer.live, bytes = runtime_observer.bytes, attempts = runtime_observer.attempts;
    XrXirValue initial = core_scalar(XR_XIR_I64, 7), receiver = {0}, alias = {0};
    force_unsupported = true; CHECK(!xr_xir_atomic_capability());
    CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_UNSUPPORTED);
    CHECK(!receiver.type && !receiver.payload && runtime_observer.attempts == attempts &&
        runtime_observer.live == live && runtime_observer.bytes == bytes);
    force_unsupported = false;
    capability_probes = 0; unsupported_probe = 3;
    size_t before_bits = bits_initializations;
    CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_UNSUPPORTED);
    CHECK(!receiver.type && !receiver.payload && runtime_observer.attempts == attempts + 1 &&
        runtime_observer.live == live && runtime_observer.bytes == bytes && bits_initializations == before_bits + 1);
    unsupported_probe = 0;
    CHECK(xr_xir_atomic_new((XrXirType)CORE_I64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
    XrXirValue fields[2] = {receiver, core_scalar(XR_XIR_BOOL, 1)}, tuple = {0}, array = {0}, optional = {0};
    CHECK(xr_xir_tuple_new((XrXirType)CORE_TUPLE, fields, 2, &owner.admission, &tuple) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)CORE_ARRAY, &receiver, 1, &owner.admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)CORE_NULLABLE, &receiver, &owner.admission, &optional) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&tuple, (XrXirType)CORE_TUPLE, &owner.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&array, (XrXirType)CORE_ARRAY, &owner.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&optional, (XrXirType)CORE_NULLABLE, &owner.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&receiver, &alias) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&receiver); xr_xir_value_drop(&tuple); xr_xir_value_drop(&array); xr_xir_value_drop(&optional);
    XrXirDomain *foreign = NULL; CHECK(xr_xir_domain_new(65536, &foreign) == XR_XIR_VALUE_OK);
    XrXirValueAdmission foreign_admission = owner.admission; foreign_admission.domain = foreign;
    CHECK(xr_xir_value_admit(&alias, (XrXirType)CORE_I64, &foreign_admission) == XR_XIR_VALUE_OK);
    XrXirAtomicRequest request = {&alias, NULL, NULL, 0, XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
    XrXirAtomicProgress progress = {0}; XrXirValue output = {0};
    CHECK(xr_xir_atomic_start(&request, &foreign_admission, &progress, &output).status == XR_XIR_RUN_OK && output.payload == 7);
    xr_xir_atomic_progress_clear(&progress); xr_xir_value_drop(&output); xr_xir_domain_drop(foreign);
    xr_xir_compile_type_arena_drop(owner.arena); owner.arena = NULL;
    xr_xir_domain_drop(owner.domain); owner.domain = NULL;
    xr_compile_resources_release(owner.context.resources); owner.context.resources = NULL;
    CHECK(xr_xir_value_valid(&alias) && compiler_observer.live && runtime_observer.live);
    xr_xir_value_drop(&alias); core_owner_free(&owner);
}
#endif
