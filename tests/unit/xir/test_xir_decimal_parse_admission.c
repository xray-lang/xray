/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_decimal_parse_admission.c - Static declaration authority and exact operand rejection
 */
#include "xir/xxir_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
static void decimal_admission_case(const char *text,XrXirStatus expected) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DECIMAL_PARSE_WORK};
    XrXirSourceRequest request={session,XR_DECIMAL_PARSE_WORK "/decimal-admission.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL,NULL};
    XrXirSourceText input={"decimal-admission.xr",text,strlen(text)};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    fprintf(stderr,"Decimal authority rejection status%u at%d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(void) {
    static const char *const cases[]={
        "export fn main()->f64? {return f64.tryParse()}",
        "export fn main()->f64? {return f64.tryParse(1)}",
        "export fn main()->f64? {return f64.tryParse(\"1\",\"2\")}",
        "export fn main()->f64? {return f64.tryParse<i64>(\"1\")}",
        "export fn main()->f64 {return f64.parse(\"1\")}",
        "export fn main()->f64? {const f64:i64=42;return f64.tryParse(\"1\")}",
        "export fn inspect(f64:i64)->f64? {return f64.tryParse(\"1\")} export fn main()->i64{return 42}"
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i)decimal_admission_case(cases[i],i<5?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE);
    puts("Five exact-signature and two reserved scalar identifier inputs rejected without published output or retained owners");return 0;
}
