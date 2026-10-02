/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stdlib_output_file_probe.inc.c - Production path failure propagation
 */
#ifdef XR_OS_WINDOWS
static void publication_path_injection(unsigned kind, size_t failure, DWORD error) {
    module_attempts = 0; module_fail_at = SIZE_MAX;
    module_os_attempts[0] = module_os_attempts[1] = 0;
    module_os_fail_at[0] = module_os_fail_at[1] = SIZE_MAX;
    module_os_fail_at[kind] = failure; module_os_error = error; module_injecting = true;
}
static void publication_path_injection_end(void) {
    module_injecting = false; module_fail_at = SIZE_MAX;
    module_os_fail_at[0] = module_os_fail_at[1] = SIZE_MAX;
}
static void publication_path_owned_free(void *pointer) {
    xr_test_stdlib_output_module_forget(pointer); xr_free(pointer);
}
static const struct { DWORD error; XrPathStatus path; XrModuleStatus module; } path_errors[] = {
    {ERROR_ACCESS_DENIED, XR_PATH_IO, XR_MODULE_IO},
    {ERROR_READ_FAULT, XR_PATH_IO, XR_MODULE_IO},
    {ERROR_NOT_ENOUGH_MEMORY, XR_PATH_OUT_OF_MEMORY, XR_MODULE_OUT_OF_MEMORY},
    {ERROR_OUTOFMEMORY, XR_PATH_OUT_OF_MEMORY, XR_MODULE_OUT_OF_MEMORY},
    {ERROR_FILENAME_EXCED_RANGE, XR_PATH_BUDGET, XR_MODULE_BUDGET},
    {ERROR_BUFFER_OVERFLOW, XR_PATH_BUDGET, XR_MODULE_BUDGET},
    {ERROR_INVALID_NAME, XR_PATH_INVALID, XR_MODULE_INVALID},
    {ERROR_FILE_NOT_FOUND, XR_PATH_NOT_FOUND, XR_MODULE_NOT_FOUND},
    {ERROR_PATH_NOT_FOUND, XR_PATH_NOT_FOUND, XR_MODULE_NOT_FOUND}
};
static void publication_source_path_faults(const XrXirSourceRequest *request) {
    publication_path_injection(0, SIZE_MAX, 0);
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    size_t calls[2] = {module_os_attempts[0], module_os_attempts[1]};
    publication_path_injection_end(); xr_xir_source_result_free(&result);
    CHECK(!module_live && !module_bytes);
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (size_t failure = 0; failure < calls[kind]; ++failure) {
            for (unsigned oom = 0; oom < 2; ++oom) {
                publication_path_injection(kind, failure, oom ? ERROR_NOT_ENOUGH_MEMORY : ERROR_ACCESS_DENIED);
                XrXirSourceDiagnostic diagnostic = {0};
                XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
                publication_path_injection_end();
                CHECK(status == (oom ? XR_XIR_OUT_OF_MEMORY : XR_XIR_IO) && diagnostic.status == status);
                CHECK(!result.checked && !result.snapshot);
                xr_xir_source_result_free(&result); CHECK(!module_live && !module_bytes);
            }
        }
    }
    printf("Source production Windows path calls stat=%zu/fullpath=%zu exact IO/OOM physical zero\n", calls[0], calls[1]);
}
static void publication_stdlib_path_faults(const char *root, const char *specifier) {
    size_t calls[2] = {0};
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (size_t pass = 0; pass == 0 || pass <= calls[kind] * 2; ++pass) {
            XrModuleResolverConfig config = {root, NULL, NULL, 0};
            XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
            XrModuleId id = {0}; char *message = NULL;
            bool oom = pass && ((pass - 1) % 2 == 0);
            publication_path_injection(kind, pass ? (pass - 1) / 2 : SIZE_MAX,
                oom ? ERROR_OUTOFMEMORY : ERROR_ACCESS_DENIED);
            XrModuleStatus status = xr_module_resolver_resolve(resolver, specifier, NULL, NULL, &id, &message);
            publication_path_injection_end();
            if (!pass) { CHECK(status == XR_MODULE_OK); calls[kind] = module_os_attempts[kind]; }
            else {
                CHECK(status == (oom ? XR_MODULE_OUT_OF_MEMORY : XR_MODULE_IO));
                CHECK(!id.canonical && !id.source_path && !id.logical_path && !id.authority.physical_root);
            }
            xr_module_id_cleanup(&id); publication_path_owned_free(message); xr_module_resolver_free(resolver);
            CHECK(!module_live && !module_bytes);
        }
    }
    printf("stdlib %s production Windows stat=%zu/fullpath=%zu exact IO/OOM physical zero\n", specifier, calls[0], calls[1]);
}
static void publication_file_probe_cases(const char *root, const char *entry, const char *source) {
    publication_stdlib_path_faults(root, "std/io/output");
    publication_stdlib_path_faults(root, "io");
    CHECK(xr_file_probe(NULL, true) == XR_PATH_INVALID);
    CHECK(xr_file_probe("", true) == XR_PATH_INVALID);
    CHECK(xr_file_probe("\xC0\x80", true) == XR_PATH_INVALID);
    CHECK(xr_file_probe(root, true) == XR_PATH_INVALID);
    char long_path[33000]; memset(long_path, 'a', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = 0;
    CHECK(xr_file_probe(long_path, true) == XR_PATH_BUDGET);
    publication_path_injection(0, SIZE_MAX, 0);
    CHECK(xr_file_probe(source, true) == XR_PATH_OK);
    CHECK(module_attempts == 1 && module_os_attempts[0] == 1);
    publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    /* Fixed API attributes cover the existing source-link/archive distinction
     * without requiring Windows symlink-creation privileges in the test host. */
    publication_path_injection(0, SIZE_MAX, 0); module_attributes_override = true;
    module_attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(xr_file_probe(source, true) == XR_PATH_OK);
    CHECK(xr_file_probe(source, false) == XR_PATH_INVALID);
    module_attributes |= FILE_ATTRIBUTE_DIRECTORY;
    CHECK(xr_file_probe(source, true) == XR_PATH_INVALID);
    CHECK(xr_file_probe(source, false) == XR_PATH_INVALID);
    module_attributes_override = false; publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    publication_path_injection(0, SIZE_MAX, 0); module_fail_at = 0;
    CHECK(xr_file_probe(source, true) == XR_PATH_OUT_OF_MEMORY);
    CHECK(module_attempts == 1 && !module_os_attempts[0]);
    publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    for (size_t error = 0; error < sizeof(path_errors) / sizeof(*path_errors); ++error) {
        publication_path_injection(0, 0, path_errors[error].error);
        CHECK(xr_file_probe(source, true) == path_errors[error].path);
        CHECK(module_attempts == 1 && module_os_attempts[0] == 1);
        publication_path_injection_end(); CHECK(!module_live && !module_bytes);
        for (size_t point = 0; point < 2; ++point) {
            publication_path_injection(1, point, path_errors[error].error);
            XrPathStatus status;
            CHECK(!xr_realpath(source, &status) && status == path_errors[error].path);
            CHECK(module_os_attempts[1] == point + 1 && module_attempts == point + 1);
            publication_path_injection_end(); CHECK(!module_live && !module_bytes);
        }
    }
    char first[2048], directory[2048], second[2048];
    CHECK(snprintf(first, sizeof(first), "%s/path-probe.xr", root) > 0);
    CHECK(snprintf(directory, sizeof(directory), "%s/path-probe", root) > 0);
    CHECK(snprintf(second, sizeof(second), "%s/path-probe/index.xr", root) > 0);
    XrWinPathStatus converted; wchar_t *wide = xr_win_utf8_path(directory, &converted); CHECK(wide);
    CHECK(CreateDirectoryW(wide, NULL)); xr_free(wide);
    FILE *file = fopen(first, "wb"); CHECK(file && !fclose(file));
    file = fopen(second, "wb"); CHECK(file && !fclose(file));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrModuleResolverConfig config = {root, NULL, NULL, 0};
    for (size_t error = 0; error < sizeof(path_errors) / sizeof(*path_errors); ++error) {
        XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
        XrModuleId id = {0}; char *message = NULL;
        publication_path_injection(0, 0, path_errors[error].error);
        XrModuleStatus status = xr_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message);
        publication_path_injection_end();
        if (path_errors[error].path == XR_PATH_NOT_FOUND) {
            CHECK(status == XR_MODULE_OK && module_os_attempts[0] == 2);
            CHECK(id.logical_path && strstr(id.logical_path, "path-probe/index.xr"));
        } else {
            CHECK(status == path_errors[error].module && module_os_attempts[0] == 1);
            CHECK(!id.canonical && !id.source_path && !id.logical_path && !id.authority.physical_root);
        }
        xr_module_id_cleanup(&id); publication_path_owned_free(message); xr_module_resolver_free(resolver);
        CHECK(!module_live && !module_bytes);
    }
    /* Every instrumented relative-resolution allocation, including UTF conversion and the
     * successful cache publication, must retain OOM rather than try index.xr. */
    size_t sites = 0;
    for (size_t pass = 0; pass == 0 || pass <= sites; ++pass) {
        XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
        XrModuleId id = {0}; char *message = NULL;
        publication_path_injection(0, SIZE_MAX, 0); module_fail_at = pass ? pass - 1 : SIZE_MAX;
        XrModuleStatus status = xr_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message);
        publication_path_injection_end();
        if (!pass) { CHECK(status == XR_MODULE_OK); sites = module_attempts; }
        else CHECK(status == XR_MODULE_OUT_OF_MEMORY && !id.canonical && !id.source_path && !id.logical_path);
        CHECK(module_os_attempts[0] <= 1);
        xr_module_id_cleanup(&id); publication_path_owned_free(message); xr_module_resolver_free(resolver);
        CHECK(!module_live && !module_bytes);
    }
    CHECK(!remove(first));
    wide = xr_win_utf8_path(first, &converted); CHECK(wide);
    CHECK(CreateDirectoryW(wide, NULL)); xr_free(wide);
    {
        XrModuleResolver *directory_resolver = xr_module_resolver_new(&config); CHECK(directory_resolver);
        XrModuleId directory_id = {0}; char *directory_message = NULL;
        publication_path_injection(0, SIZE_MAX, 0);
        CHECK(xr_module_resolver_resolve(directory_resolver, "./path-probe", entry, &authority,
            &directory_id, &directory_message) == XR_MODULE_INVALID);
        CHECK(module_os_attempts[0] == 1 && !directory_id.canonical && !directory_id.source_path);
        publication_path_injection_end();
        xr_module_id_cleanup(&directory_id); publication_path_owned_free(directory_message);
        xr_module_resolver_free(directory_resolver); CHECK(!module_live && !module_bytes);
    }
    wide = xr_win_utf8_path(first, &converted); CHECK(wide); CHECK(RemoveDirectoryW(wide)); xr_free(wide);
    XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
    XrModuleId id = {0}; char *message = NULL;
    publication_path_injection(0, SIZE_MAX, 0);
    CHECK(xr_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message) == XR_MODULE_OK);
    CHECK(module_os_attempts[0] == 2 && strstr(id.logical_path, "path-probe/index.xr"));
    publication_path_injection_end();
    xr_module_id_cleanup(&id); publication_path_owned_free(message); xr_module_resolver_free(resolver);
    CHECK(!module_live && !module_bytes && !remove(second));
    wide = xr_win_utf8_path(directory, &converted); CHECK(wide);
    CHECK(RemoveDirectoryW(wide)); xr_free(wide);
    printf("typed file probe: 9 fixed Windows statuses; 2 realpath points; relative allocations=%zu; only NOT_FOUND advances\n", sites);
}
#else
static void publication_source_path_faults(const XrXirSourceRequest *request) { (void)request; }
static void publication_file_probe_cases(const char *root, const char *entry, const char *source) {
    (void)entry; CHECK(xr_file_probe(root, true) == XR_PATH_INVALID);
    CHECK(xr_file_probe(source, true) == XR_PATH_OK);
}
#endif
