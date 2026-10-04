/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_iteration_cases.h - Array iteration checks at the definition owner
 *
 * KEY CONCEPT:
 *   A concrete instantiation never supplies missing iteration authority.
 */
#ifndef XIR_SOURCE_ITERATION_CASES_H
#define XIR_SOURCE_ITERATION_CASES_H
static void source_iteration_definition_cases(XrXirSourceRequest *request) {
 static const struct {const char *source; XrXirStatus status; const char *message; bool prefix;} cases[]={
 {"fn unused(){for(x in [1]){x=2}}",3,"assignment requires a mutable binding",false},
 {"fn unused(){for(x,x in [1]){}}",1,"for-in binding names must be distinct",false},
 {"fn unused<T>(xs:T){for(x in xs){}}",3,"for-in requires a proved built-in Array value",false},
 {"final class Array<T>{constructor(){}}\nfn unused(xs:Array<i64>){for(x in xs){}}",3,"for-in requires a proved built-in Array value",false},
 {"fn unused(){for(x:i64 in [1]){}}",1,"module graph build failed",false},
 {"fn unused(){for((i,x) in [1]){}}",1,"Array for-in requires an unlabelled single or direct pair binding without annotation",false},
 {"fn unused(){outer:for(x in [1]){}}",1,"Array for-in requires an unlabelled single or direct pair binding without annotation",false},
 {"fn unused(){for(x in \"abc\"){}}",3,"for-in requires a proved built-in Array value",false},
 {"final class C{const n:i64 constructor(xs:Array<i64>){for(x in xs){this.n=x}}}",4,"write may overwrite already initialized const storage",false},
 {"fn unused<T:Sendable>(xs:Array<T>){for(x in xs){const y=x}}",0,"",false},
 {"fn unused(){for(_ in [1]){const x=_}}",1,"module graph build failed",false},
 {"fn unused(){for(x in [1]){const y=fn()->i64{return x}}}",0,"",false},
 {"fn unused(){const f=fn()->i64{var n=0;for(x in [1,2]){n+=x};return n}}",0,"",false},
 {"final class C{n:i64 constructor(xs:Array<i64>){for(x in xs){this.n=x};this.n=41}}",0,"",false},
 {"final class C{n:i64 constructor(xs:Array<i64>){for(x in xs){this.n=x}}}",XR_XIR_BAD_VALUE,"read requires storage initialized on every incoming path",false},
 };
 for(uint32_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
  if (i == 4) source_query_syntax_boundary(request, cases[i].source, "for-in bindings do not accept type annotations");
  if (i == 10) source_query_syntax_boundary(request, cases[i].source, "expected expression");
  write_source(request->entry_path,cases[i].source);
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
  bool message=cases[i].prefix ? !strncmp(diagnostic.message,cases[i].message,strlen(cases[i].message)) : !strcmp(diagnostic.message,cases[i].message);
  if(status!=cases[i].status || !message)fprintf(stderr,"iteration case %u: %u %s\n",i,status,diagnostic.message);
  CHECK(status==cases[i].status && message);
  if (i == 4 || i == 10) source_query_syntax_graph_diagnostic(&diagnostic);
  CHECK((result.checked!=NULL)==(status==XR_XIR_OK));
  if (status == XR_XIR_OK) CHECK(result.snapshot && xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        else CHECK(!result.snapshot);
  xr_xir_compile_source_result_free(&result);
 }
}
#endif // XIR_SOURCE_ITERATION_CASES_H
