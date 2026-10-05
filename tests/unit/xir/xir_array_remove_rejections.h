/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_rejections.h - Definition and receiver permissions
 *
 * KEY CONCEPT:
 *   Rejected complete Source requests publish neither Checked nor snapshots.
 */
#ifndef XIR_ARRAY_REMOVE_REJECTIONS_H
#define XIR_ARRAY_REMOVE_REJECTIONS_H
static void remove_rejections(void) {
    static const char *const cases[] = {
        "const_pop", "const_shift", "read_pop", "read_shift", "temporary_pop", "temporary_shift",
        "extra_pop", "extra_shift", "typearg_pop", "typearg_shift", "flatten_pop", "flatten_shift",
        "const_field", "private_field", "ordinary_generic", "Unit_element",
        "constructor_private", "constructor_missing", "constructor_type", "constructor_typeargs", "constructor_ref"
    };
    static const char *const reasons[] = {
        "value mutation requires a mutable named root", "value mutation requires a mutable named root",
        "value mutation requires a mutable named root", "value mutation requires a mutable named root",
        "value mutation requires a mutable named root", "value mutation requires a mutable named root",
        "Array removal requires no explicit arguments", "Array removal requires no explicit arguments",
        "Array removal requires no explicit arguments", "Array removal requires no explicit arguments",
        "expression cannot satisfy its declared type", "expression cannot satisfy its declared type",
        "field access is not permitted", "field access is not permitted",
        "expression cannot satisfy its declared type", "nullable element must be an ordinary value type",
        "constructor arity or authority mismatch", "missing required call argument",
        "expression cannot satisfy its declared type", "nominal type requires its exact explicit arguments",
        "constructor argument requires a value"
    };
    _Static_assert(sizeof(cases)/sizeof(cases[0]) == sizeof(reasons)/sizeof(reasons[0]), "Every original input has one independent reason");
    for (unsigned n = 0; n < sizeof(cases)/sizeof(cases[0]); ++n) {
        instance_compile_zero(); instance_compile_fail_at = SIZE_MAX;
        XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
        const XrCompileResourceLimits limits = remove_limits();
        CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline = remove_stats(&context).live_bytes;
        XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
        char name[1024]; CHECK(snprintf(name, sizeof(name), "%s/rejected/%s/root.xr", XR_REMOVE_FIXTURES, cases[n]) > 0);
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REMOVE_FIXTURES};
        XrXirSourceRequest request = {session, name, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *path = NULL;
        XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &path);
        if (status != XR_XIR_BAD_TYPE) fprintf(stderr,"rejection %s status%u line%d %s\n",cases[n],status,diagnostic.line,diagnostic.message);
        CHECK(status == XR_XIR_BAD_TYPE && diagnostic.status == status && !result.checked && !result.snapshot);
        CHECK(!strcmp(diagnostic.message, reasons[n]));
        printf("remove negative%u %s status%u reason=%s\n", n, cases[n], status, diagnostic.message);
        xr_compile_resources_free(path); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
        CHECK(remove_stats(&context).live_bytes == baseline); xr_compile_resources_release(context.resources);
        instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
    }
}
#endif // XIR_ARRAY_REMOVE_REJECTIONS_H
