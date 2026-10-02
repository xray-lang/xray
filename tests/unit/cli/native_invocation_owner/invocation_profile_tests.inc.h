/* The unique profile derivation uses live Source/SDK/process owners.
 * No command is executed here. */
static void profile_domain_matrix(const XrXirInvocationRequest *request) {
    XrCompileResources *resources = xr_compile_native_projection_context(request->projection)->resources;
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirInvocation derived = {0}; derived.context.resources = resources;
    derived.io = xr_compile_io_policy(resources); derived.limits.dependencies.path_bytes = 32768;
    for (unsigned i = 0; i < 3; ++i) CHECK(xtc_process_view(i < 2 ? request->compile[i].process : request->link,
        &derived.commands[i]) == XTC_PROCESS_OK);
    CHECK(invocation_profile_derive(&derived, request));
    const char *leaves[] = {"kernel32.dll", "drivers/etc/hosts"};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL; XtcXirFileLease *lease = NULL;
        if (i < 2) CHECK(xr_path_join_owned(&derived.io, derived.profile.paths[9], leaves[i], &path) == XR_OS_IO_OK);
        CHECK(xtc_xir_file_lease_open(resources, i < 2 ? path : derived.commands[0].executable, &lease) == XR_XIR_TARGET_OK);
        const char *canonical = xtc_xir_file_lease_facts(lease)->path;
        /* Each independent decision borrows the one live derived root set. */
        XrXirInvocation image = derived, header = derived;
        CHECK(invocation_profile_contains(&image, canonical, true) == (i != 1));
        if (i == 1) CHECK(image.status == XR_XIR_INVOCATION_UNSUPPORTED);
        CHECK(!invocation_profile_contains(&header, canonical, false) && header.status == XR_XIR_INVOCATION_UNSUPPORTED);
        xtc_xir_file_lease_free(lease); xr_compile_resources_free(path);
    }
    invocation_profile_free(&derived.profile);
    CHECK(runtime_live == live && runtime_bytes == bytes);
    puts("real leased provider/system files: direct images allowed, nested system image and out-of-root headers rejected PASS");
}
/* Exercise the one production derivation without executing a child. */
static XrXirInvocationStatus profile_derive_status(const XrXirInvocationRequest *request) {
    XrXirInvocation derived = {0};
    derived.context = *xr_compile_native_projection_context(request->projection);
    derived.io = xr_compile_io_policy(derived.context.resources); derived.limits = request->limits;
    for (unsigned i = 0; i < 3; ++i) CHECK(xtc_process_view(i < 2 ? request->compile[i].process : request->link,
        &derived.commands[i]) == XTC_PROCESS_OK);
    bool okay = invocation_profile_derive(&derived, request);
    CHECK(okay == (derived.status == XR_XIR_INVOCATION_OK));
    invocation_profile_free(&derived.profile); return derived.status;
}
static void profile_derivation_matrix(const XrXirInvocationRequest *request) {
    XrCompileResources *resources = xr_compile_native_projection_context(request->projection)->resources;
    XrCompileResourceStats baseline, after;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t live = runtime_live, bytes = runtime_bytes, first = runtime_attempts;
    DWORD handles = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    profile_os_calls = 0;
    CHECK(profile_derive_status(request) == XR_XIR_INVOCATION_OK);
    size_t allocations = runtime_attempts - first, calls = profile_os_calls;
    for (size_t i = 0; i < allocations; ++i) {
        first = runtime_attempts; runtime_fail_at = first + i;
        CHECK(profile_derive_status(request) == XR_XIR_INVOCATION_OUT_OF_MEMORY);
        CHECK(runtime_attempts == first + i + 1); runtime_fail_at = SIZE_MAX;
        CHECK(runtime_live == live && runtime_bytes == bytes);
        CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK && after.live_bytes == baseline.live_bytes);
    }
    const DWORD errors[] = {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY};
    for (unsigned error = 0; error < 3; ++error) for (size_t point = 0; point < calls; ++point) {
        profile_os_calls = 0; profile_os_failure = point; profile_os_error = errors[error];
        XrXirInvocationStatus expected = error ? XR_XIR_INVOCATION_OUT_OF_MEMORY : XR_XIR_INVOCATION_IO;
        CHECK(profile_derive_status(request) == expected);
        CHECK(profile_os_calls == point + 1 && runtime_live == live && runtime_bytes == bytes);
    }
    profile_os_failure = SIZE_MAX; profile_packaged = true;
    CHECK(profile_derive_status(request) == XR_XIR_INVOCATION_UNSUPPORTED);
    profile_packaged = false;
    CHECK(SetDllDirectoryW(L"C:\\Windows"));
    CHECK(profile_derive_status(request) == XR_XIR_INVOCATION_UNSUPPORTED);
    CHECK(SetDllDirectoryW(L""));
    CHECK(profile_derive_status(request) == XR_XIR_INVOCATION_OK);
    CHECK(SetDllDirectoryW(NULL));
    profile_domain_matrix(request);
    XrXirInvocationRequest overlap = *request; overlap.output_directory = request->msvc.vc_include;
    CHECK(profile_derive_status(&overlap) == XR_XIR_INVOCATION_UNSUPPORTED);
    DWORD final_handles = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &final_handles));
    CHECK(final_handles == handles && runtime_live == live && runtime_bytes == bytes);
    printf("profile derivation %zu actual OOM, %zu host calls x IO/two OOM, package/DLL overrides, exact physical baseline PASS\n", allocations, calls);
}
