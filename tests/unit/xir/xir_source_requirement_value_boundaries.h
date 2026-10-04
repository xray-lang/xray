/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_boundaries.h - Exact synthesized function capacity
 *
 * KEY CONCEPT:
 *   Reserved closures and actual bound helpers retain separate stable identities.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_BOUNDARIES_H
#define XIR_SOURCE_REQUIREMENT_VALUE_BOUNDARIES_H
static const char requirement_value_mixed_source[] =
    "interface I{map<U:Sendable>(value:U)->U}\n"
    "struct S implements I{value:i64=0;map<V:Sendable>(value:V)->V{return value}}\n"
    "fn mixed<T:I>(receiver:T,factory:fn(T)->fn(i64)->i64=fn(value:T)->fn(i64)->i64{return value.map<i64>})->i64{"
    "const initial=factory(receiver);"
    "const run=fn(value:i64)->i64{const saved=receiver.map<i64>;return saved(value)};"
    "defer {const saved=receiver.map<i64>;};return initial(run(41))}\n"
    "const initialized=mixed<S>(S())\n"
    "export fn genericMethodNumber()->i64{return initialized}\n"
    "export fn genericMethodText()->string{return \"mapped\"}\n";
static void source_requirement_value_boundaries(XrXirSourceRequest *request) {
    write_source(request->entry_path,requirement_value_mixed_source);
    XrXirSourceResult baseline={0}; XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check(request, &baseline, &diagnostic, NULL)==XR_XIR_OK && baseline.checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(baseline.checked);
    uint32_t required=module->function_count, helpers=0, closures=0, cleanups=0, defaults=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length>=12 && !memcmp(function->name,"$requirement",12)) {
            ++helpers; CHECK(module->generics && module->generics[f].parameter_count==1);
            CHECK(function->parameter_count==2 && function->result==XR_XIR_I64);
            CHECK(function->parameters[0]==XR_XIR_TYPE_PARAMETER_BASE && function->parameters[1]==XR_XIR_I64);
        }
        closures+=function->name_length>=8 && !memcmp(function->name,"$closure",8);
        cleanups+=function->name_length>=8 && !memcmp(function->name,"$cleanup",8);
        defaults+=function->name_length==17 && !memcmp(function->name,"$argument_default",17);
    }
    CHECK(helpers==3 && closures==2 && cleanups==1 && defaults==1 && required==15);
    CHECK(module->declarations->entry_function==required-helpers-1);
    const XrXirFunction *entry=&module->functions[module->declarations->entry_function];
    CHECK(entry->name_length==6 && !memcmp(entry->name,"$entry",6) && !entry->parameter_count && entry->result==XR_XIR_I64);
    xr_xir_compile_source_result_free(&baseline);
    bool capacity_matched = true;
    const XrXirCompileContext original = *request->context;
    for (uint32_t exact=0;exact<2;++exact) {
        XrXirCompileContext context = *request->context; context.limits.functions = required - (exact ? 0u : 1u);
        const XrXirCompileContext frozen = context;
        const XrCompileResourceStats before = stage_stats(&context);
        XrXirSourceRequest limited = *request; limited.context = &context;
        XrXirSourceResult result={0}; diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=xr_xir_compile_source_check(&limited, &result, &diagnostic, NULL);
        fprintf(stderr, "requirement capacity: exact=%u functions=%u status=%u location=%d:%d\n",
            exact, context.limits.functions, status, diagnostic.line, diagnostic.column);
        if (exact) capacity_matched &= status==XR_XIR_OK && result.checked && result.snapshot &&
            xr_xir_compile_artifact_module(result.checked)->function_count==required;
        else capacity_matched &= status==XR_XIR_BUDGET && !result.checked && !result.snapshot &&
            diagnostic.status == XR_XIR_BUDGET && !diagnostic.message[0];
        CHECK(status == XR_XIR_OK || (!result.checked && !result.snapshot));
        xr_xir_compile_source_result_free(&result);
        const XrCompileResourceStats after = stage_stats(&context);
        CHECK(after.live_bytes == before.live_bytes && after.work > before.work &&
            after.allocated_bytes > before.allocated_bytes);
        CHECK(!memcmp(&context, &frozen, sizeof(context)) &&
            !memcmp(request->context, &original, sizeof(original)));
    }
    CHECK(capacity_matched);
    source_generic_requirement_run(request,requirement_value_mixed_source);
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_BOUNDARIES_H
