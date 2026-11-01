/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * client.c - An authentic previous-header rejection customer
 *
 * KEY CONCEPT:
 *   The Windows x64 customer keeps its original public prototype and empty
 *   output contract. The test makes no cross-platform prototype guarantee.
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__cplusplus)
static_assert(XRAY_SOURCE_API_VERSION==4 && XRAY_SOURCE_EXECUTION_ABI_VERSION==1,
    "The customer must use the unchanged previous public header");
#else
_Static_assert(XRAY_SOURCE_API_VERSION==4 && XRAY_SOURCE_EXECUTION_ABI_VERSION==1,
    "The customer must use the unchanged previous public header");
#endif
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"previous Source header %d: %s\n",__LINE__,#c);exit(1);}} while (0)
typedef XrSourceStatus (*PreviousExecute)(XrSourceExecution *,XrSourceArtifact *,
    const XrSourceCallOptions *,XrSourceResult **,XrSourceEvent *);
int main(int argc,char **argv) {
    CHECK(argc==2);
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=argv[1];
    XrSourceInput input;memset(&input,0,sizeof(input));input.kind=XR_SOURCE_INPUT_TEXT;
    input.value.text.namespace_id="api4_previous";input.value.text.bytes="40 + 2\n";
    input.value.text.length=strlen(input.value.text.bytes);
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure build_failure;
    memset(&build_failure,0,sizeof(build_failure));
    CHECK(xray_source_build(&input,&options,&artifact,&build_failure,&diagnostic)==XR_SOURCE_OK);
    CHECK(artifact && !diagnostic);
    XrSourceExecutionOptions execution_options=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&execution_options,&execution)==XR_SOURCE_OK);
    XrSourceCallOptions call;
    CHECK(xray_source_call_options_init(&call,sizeof(call))==XR_SOURCE_OK);
    /* The current initializer can fill the previous same-sized struct. Do not
     * substitute an edited version field for this actual old-header call. */
    CHECK(call.api_version==2u);
    struct Guard {uint64_t before;XrSourceResult *result;uint64_t after;} owner;
    owner.before=UINT64_C(0x1122334455667788);owner.result=NULL;owner.after=UINT64_C(0x8877665544332211);
    XrSourceEvent failure,prior;memset(&failure,0xa5,sizeof(failure));memcpy(&prior,&failure,sizeof(prior));
    PreviousExecute execute=xray_source_artifact_execute;
    CHECK(execute(execution,artifact,&call,&owner.result,&failure)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(!owner.result && owner.before==UINT64_C(0x1122334455667788) && owner.after==UINT64_C(0x8877665544332211));
    CHECK(!memcmp(&failure,&prior,sizeof(failure)));
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    puts("Previous Source API4 header: old execute rejected with empty owners PASS");return 0;
}
