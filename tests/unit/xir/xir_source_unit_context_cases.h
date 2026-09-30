/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_unit_context_cases.h - Explicit Unit result contexts remain present
 *
 * KEY CONCEPT:
 *   Unit functions forward once; present Unit cannot accept a non-Unit result.
 */
static void source_unit_context_cases(XrXirSourceRequest *request) {
 source_generic_requirement_run(request,
 "var count=0\nfn nop(){count+=1}\nfn forwarded(){return nop()}\n"
 "export fn genericMethodNumber()->i64{forwarded();return count+40}\n"
 "export fn genericMethodText()->string{return \"mapped\"}\n");
 source_generic_requirement_run(request,
 "var count=0\nfn nop(){count+=1}\n"
 "fn grouped(){return (nop())}\n"
 "fn conditional(){return true ? grouped() : nop()}\n"
 "fn matched(){return match(true){true->{conditional()},false->nop()}}\n"
 "fn invoke(f:fn()){return f()}\n"
 "export fn genericMethodNumber()->i64{matched();invoke(fn(){return matched()});return count+39}\n"
 "export fn genericMethodText()->string{return \"mapped\"}\n");
 const char *bad_sources[]={
  "fn bad(){return []}\n",
  "fn bad(){return ([])}\n",
  "fn bad(){return true ? [] : []}\n",
  "fn bad(){return match(true){true->[],false->[]}}\n",
  "fn nop(){}\nfn bad(){return true ? nop() : 41}\n",
  "fn bad(){return fn(x){return x}}\n",
  "fn bad(){const value:()=41}\n",
  "fn unused<T>(){}\nfn bad(){unused<()>()}\n",
  "fn bad(){const values:Array<()>=[]}\n"
 };
 const char *messages[]={
  "Array literal cannot satisfy a non-Array context",
  "Array literal cannot satisfy a non-Array context",
  "Array literal cannot satisfy a non-Array context",
  "Array literal cannot satisfy a non-Array context",
  "expression cannot satisfy its declared type",
  "expression cannot satisfy its declared type",
  "expression cannot satisfy its declared type",
  "type argument does not prove the declared constraint",
  "constructed XIR failed checking at function 4294967295 block 4294967295 instruction 4294967295"
 };
 for (unsigned i=0;i<sizeof(bad_sources)/sizeof(bad_sources[0]);++i) {
  write_source(request->entry_path,bad_sources[i]);
  XrXirSourceResult rejected={0}; XrXirSourceDiagnostic error={0};
  CHECK(xr_xir_source_check(request,&rejected,&error)==XR_XIR_BAD_TYPE&&!rejected.checked);
  if(strcmp(error.message,messages[i])) fprintf(stderr,"Unit helper case %u: %s\n",i,error.message);
  CHECK(!strcmp(error.message,messages[i]));xr_xir_source_result_free(&rejected);
 }
 write_source(request->entry_path,"fn bad(){return 41}\n");
 XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
 CHECK(xr_xir_source_check(request,&result,&diagnostic)==XR_XIR_BAD_TYPE&&!result.checked);
 CHECK(!strcmp(diagnostic.message,"expression cannot satisfy its declared type"));xr_xir_source_result_free(&result);
}
