/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_contextual_lambda_cases.h - Authentic complete lambda contexts
 *
 * KEY CONCEPT:
 *   Prior evidence supplies a full callable signature; closure body checking
 *   retains the caller binder and proves the original callable contract.
 */
#ifndef XIR_SOURCE_CONTEXTUAL_LAMBDA_CASES_H
#define XIR_SOURCE_CONTEXTUAL_LAMBDA_CASES_H
/* Include after generic_requirement_cases and witness_promises helpers. */
static void source_contextual_lambda_cases(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "fn apply<A>(seed:A)->A{const f:fn(A)->A=fn(v){return v};return f(seed)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64>(41)}\n"
        "export fn genericMethodText()->string{return apply<string>(\"mapped\")}\n");
    source_generic_requirement_run(request,
        "export fn genericMethodNumber()->i64{const offset=1;const f:fn(i64)->i64=fn(v){return v+offset};return f(40)}\n"
        "export fn genericMethodText()->string{const f:fn()->Array<string>=fn(){return []};const empty=f();return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "fn choose<T,U>(seed:T,body:fn(T)->T,tag:U)->T{return body(seed)}\n"
        "export fn genericMethodNumber()->i64{return choose(41,fn(v){return v},\"tag\")}\n"
        "export fn genericMethodText()->string{return choose(\"mapped\",fn(v){return v},0)}\n");
    const char *rejected[]={
        "fn bad(){const f=fn(v){return v}}\n",
        "fn bad(){const f:fn(i64)->i64=fn(a,b){return a}}\n",
        "fn bad(){const f:fn(i64)->i64=fn(v:string){return 41}}\n",
        "fn bad(){const f:fn(i64)->i64=fn(v)->string{return \"wrong\"}}\n",
        "fn bad(){const f:fn(i64)->i64=fn(v){return \"wrong\"}}\n",
        "fn consume<T>(f:fn(T)->T)->i64{return 41}\nfn bad()->i64{return consume(fn(v){return v})}\n"
    };
    const char *expected_messages[]={
        "closure parameter requires an annotation or complete callable context", /* no callable context or annotation */
        "closure arity differs from its callable context", /* wrong arity before closure creation */
        "callable conversion may only discard its top-level promise", /* explicit parameter signature mismatch */
        "callable conversion may only discard its top-level promise", /* explicit return signature mismatch */
        "expression cannot satisfy its declared type", /* body return fails contextual i64 */
        "closure parameter requires an annotation or complete callable context", /* lambda own type has no argument or result evidence */
    };
    _Static_assert(sizeof(expected_messages)/sizeof(*expected_messages)==sizeof(rejected)/sizeof(*rejected), "Every rejection needs an exact diagnostic");
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if (status!=XR_XIR_BAD_TYPE) fprintf(stderr,"contextual lambda rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked);
        if (strcmp(diagnostic.message,expected_messages[i]))
            fprintf(stderr,"inference diagnostic %u: expected '%s', got '%s'\n",
                i,expected_messages[i],diagnostic.message);
        CHECK(strcmp(diagnostic.message,expected_messages[i])==0);
        if (result.snapshot) CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
        xr_xir_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_CONTEXTUAL_LAMBDA_CASES_H
