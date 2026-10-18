/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_constructed_rejects.c - Scalar capability does not weaken Task typing
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task constructed reject FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
int main(int argc, char **argv) {
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2],"bad-value")));
    XrXirStatus expected=argc==3?XR_XIR_BAD_VALUE:XR_XIR_BAD_TYPE;
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/rejected/%s.xr", XR_TASK_CONSTRUCTED_FIXTURES, argv[1]) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_CONSTRUCTED_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    xr_compile_session_free(session);
    if (status!=expected) fprintf(stderr,"Task reject status=%u expected=%u %s\n",status,expected,diagnostic.message);
    CHECK(status == expected && !result.checked && !result.snapshot);
    CHECK(diagnostic.message[0]);
    printf("TASK_CONSTRUCTED_REJECT %s status=%u noArtifact %s\n", argv[1], status, diagnostic.message);
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes); return 0;
}
