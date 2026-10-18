/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_policy.inc.c - Atomic observed-VM exception filter admission
 */
static XrXirValueStatus debug_native_source_coverage(XrXirVmDebug *debug) {
    if (!debug->source_memory || !debug->native_code || debug->native_code->count!=debug->source_functions)
        return XR_XIR_VALUE_UNSUPPORTED;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    for(uint32_t f=0;f<module->function_count;++f) {
        if (debug->native_code->offsets[f]==UINT32_MAX) continue;
        const XrXirFunction *body=&module->functions[f];
        if (!xr_xir_domain_work(debug->domain,(uint64_t)body->instruction_count*(sizeof(XrXirInstruction)+sizeof(DebugSourcePoint))+1))
            return XR_XIR_VALUE_LIMIT;
        for(uint32_t i=0;i<body->instruction_count;++i) {
            XrXirOp op=body->instructions[i].op;
            if ((op==XR_XIR_THROW || op==XR_XIR_JUMP || op==XR_XIR_CLEANUP_ERROR) &&
                !(debug_source_point(debug,f,i).flags&DEBUG_SOURCE_AUTHORED)) return XR_XIR_VALUE_UNSUPPORTED;
        }
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus debug_exception_coverage(XrXirVmDebug *debug) {
    if (!debug->source_memory) return XR_XIR_VALUE_UNSUPPORTED;
    if (debug->mixed) {
        XrXirValueStatus status=debug_native_source_coverage(debug);if (status!=XR_XIR_VALUE_OK) return status;
    }
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    uint64_t instructions=debug->source_offsets[debug->source_functions],throws=debug->source_throw_offsets[debug->source_functions];
    if (!xr_xir_domain_work(debug->domain,instructions*35+throws*33+debug->source_functions)) return XR_XIR_VALUE_LIMIT;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            XrXirOp op=body->instructions[i].op;
            bool invoked=op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT;
            bool call=invoked || op==XR_XIR_CALL || op==XR_XIR_CALL_INDIRECT;
            if ((call || op==XR_XIR_THROW) && !(debug_source_point(debug,f,i).flags&DEBUG_SOURCE_AUTHORED)) return XR_XIR_VALUE_UNSUPPORTED;
            if (invoked && !debug_source_route(debug,f,i)) return XR_XIR_VALUE_UNSUPPORTED;
        }
        for (uint32_t i=debug->source_throw_offsets[f];i<debug->source_throw_offsets[f+1];++i) {
            uint32_t at=debug->source_throws[i].instruction;
            if (body->instructions[at].op!=XR_XIR_THROW && !debug_source_route(debug,f,at)) return XR_XIR_VALUE_UNSUPPORTED;
        }
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_break_on_throw(XrXirVmDebug *debug,uint32_t filters) {
    if (!debug_valid(debug) || !debug->artifact || debug->disabled || (filters&~7u) ||
        ((debug->count || debug->calls.count) && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
    if (debug->mixed && (filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL)) {
        XrXirValueStatus status=debug_native_source_coverage(debug);if (status!=XR_XIR_VALUE_OK) return status;
    }
    if (filters && !(filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL)) {
        XrXirValueStatus status=debug_exception_coverage(debug);if (status!=XR_XIR_VALUE_OK) return status;
    }
    debug->exception_filters=filters;return XR_XIR_VALUE_OK;
}
static uint32_t debug_exception_filter(uint32_t filters,XirVmDebugExceptionRouteKind route) {
    if (filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL) return XR_XIR_VM_DEBUG_EXCEPTION_ALL;
    if (route==XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED) return filters&XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT;
    if (route==XIR_VM_DEBUG_EXCEPTION_ROUTE_ROOT_ESCAPE || route==XIR_VM_DEBUG_EXCEPTION_ROUTE_TASK_ESCAPE)
        return filters&XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT;
    return 0;
}
