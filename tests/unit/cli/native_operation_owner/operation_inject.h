/* Interpose only the new composition boundary. All owners and allocations are
 * real. The explicit pre-execution stop does not qualify native execution. */
static bool operation_stop_before_native, operation_hold_workspace;
static size_t operation_direct_count, operation_direct_fail = SIZE_MAX, operation_run_calls;
static size_t operation_workspace_closes;
static XrCompileResourceStatus operation_alloc(XrCompileResources *resources, size_t bytes, void **out) {
    bool fail = operation_direct_count++ == operation_direct_fail;
    size_t saved = runtime_fail_at;
    if (fail) runtime_fail_at = runtime_attempts;
    XrCompileResourceStatus status = xr_compile_resources_alloc(resources, bytes, out);
    runtime_fail_at = saved; return status;
}
static XrCompileResourceStatus operation_calloc(XrCompileResources *resources, size_t count, size_t size, void **out) {
    bool fail = operation_direct_count++ == operation_direct_fail;
    size_t saved = runtime_fail_at;
    if (fail) runtime_fail_at = runtime_attempts;
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, count, size, out);
    runtime_fail_at = saved; return status;
}
static XrXirInvocationStatus operation_invoke(const XrXirInvocationRequest *request,
    XrXirInvocation **out, XrXirInvocationDiagnostic *diagnostic) {
    ++operation_run_calls;
    if (operation_stop_before_native) {
        CHECK(!*out && request->launcher && request->launcher_length);
        /* Independently check the SDK launcher; the previous invocation fixture
         * used a different test consumer and cannot satisfy this expectation. */
        static const char expected[] = "return xr_xir_host_main(&XIR_SDK_PROGRAM_SYMBOL);";
        bool found = false;
        for (size_t i = 0; i + sizeof(expected) - 1 <= request->launcher_length; ++i)
            if (!memcmp((const char *)request->launcher + i, expected, sizeof(expected) - 1)) { found = true; break; }
        CHECK(found);
        for (unsigned i = 0; i < 3; ++i) {
            XrProcessView view;
            CHECK(xtc_process_view(i < 2 ? request->compile[i].process : request->link, &view) == XTC_PROCESS_OK);
            CHECK(view.env_count == 3 && !strcmp(view.env_keys[0], "SystemRoot") &&
                !strcmp(view.env_keys[1], "TEMP") && !strcmp(view.env_keys[2], "TMP"));
            CHECK(!strcmp(view.cwd, request->input_directory));
            CHECK(!strcmp(view.env_values[1], request->output_directory) && !strcmp(view.env_values[2], request->output_directory));
            CHECK(i == 2 || (view.argc == (i ? 27u : 26u) && !strcmp(view.argv[13], "/X")));
        }
        *diagnostic = (XrXirInvocationDiagnostic){XR_XIR_INVOCATION_NO_STAGE, XR_XIR_INVOCATION_NO_PASS,
            XR_XIR_INVOCATION_SELF, XR_XIR_INVOCATION_IO, 0};
        return XR_XIR_INVOCATION_IO;
    }
    return xtc_xir_invocation_run(request, out, diagnostic);
}
static XtcXirWorkspaceStatus operation_workspace_close(XtcXirWorkspace **owner, uint32_t steps) {
    ++operation_workspace_closes;
    if (*owner && operation_hold_workspace) return XTC_XIR_WORKSPACE_IO;
    return xtc_xir_workspace_close(owner, steps);
}
#define xr_compile_resources_alloc operation_alloc
#define xr_compile_resources_calloc operation_calloc
#define xtc_xir_invocation_run operation_invoke
#define xtc_xir_workspace_close operation_workspace_close
#include "app/toolchain/xtc_xir_native_operation.c"
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xtc_xir_invocation_run
#undef xtc_xir_workspace_close
