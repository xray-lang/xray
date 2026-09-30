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
 write_source(request->entry_path,"fn bad(){return 41}\n");
 XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
 CHECK(xr_xir_source_check(request,&result,&diagnostic)==XR_XIR_BAD_TYPE&&!result.checked);
 CHECK(!strcmp(diagnostic.message,"expression cannot satisfy its declared type"));xr_xir_source_result_free(&result);
}
