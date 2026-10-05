/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stdlib_output_file_probe.inc.c - Production path failure propagation
 */
#ifdef XR_OS_WINDOWS
static XrCompileResources *publication_path_resources;
static XrOsIoPolicy publication_path_policy;
static const XrOsIoPolicy *publication_current_path_policy(void) {
    if (!publication_path_resources) publication_path_policy=xr_compile_io_policy(publication_context->resources);
    return &publication_path_policy;
}
static void publication_path_injection(unsigned kind, size_t failure, DWORD error) {
    CHECK(!publication_path_resources);
    CHECK(xr_compile_resources_new(&publication_caps,&publication_path_resources)==XR_COMPILE_RESOURCE_OK);
    publication_path_policy=xr_compile_io_policy(publication_path_resources);
    module_attempts = 0; module_fail_at = SIZE_MAX;
    module_os_attempts[0] = module_os_attempts[1] = 0;
    module_os_fail_at[0] = module_os_fail_at[1] = SIZE_MAX;
    module_os_fail_at[kind] = failure; module_os_error = error; module_injecting = true;
}
static void publication_path_injection_end(void) {
    module_injecting = false; module_fail_at = SIZE_MAX;
    module_os_fail_at[0] = module_os_fail_at[1] = SIZE_MAX;
    xr_compile_resources_release(publication_path_resources);publication_path_resources=NULL;
}
static void publication_path_owned_free(void *pointer) {
    xr_compile_resources_free(pointer);
}
static const struct { DWORD error; XrOsIoStatus path; XrModuleStatus module; } path_errors[] = {
    {ERROR_ACCESS_DENIED, XR_OS_IO_IO, XR_MODULE_IO},
    {ERROR_READ_FAULT, XR_OS_IO_IO, XR_MODULE_IO},
    {ERROR_NOT_ENOUGH_MEMORY, XR_OS_IO_OUT_OF_MEMORY, XR_MODULE_OUT_OF_MEMORY},
    {ERROR_OUTOFMEMORY, XR_OS_IO_OUT_OF_MEMORY, XR_MODULE_OUT_OF_MEMORY},
    {ERROR_FILENAME_EXCED_RANGE, XR_OS_IO_BUDGET, XR_MODULE_BUDGET},
    {ERROR_BUFFER_OVERFLOW, XR_OS_IO_BUDGET, XR_MODULE_BUDGET},
    {ERROR_INVALID_NAME, XR_OS_IO_BAD_ARGUMENT, XR_MODULE_INVALID},
    {ERROR_FILE_NOT_FOUND, XR_OS_IO_NOT_FOUND, XR_MODULE_NOT_FOUND},
    {ERROR_PATH_NOT_FOUND, XR_OS_IO_NOT_FOUND, XR_MODULE_NOT_FOUND}
};
static void publication_source_path_faults(const XrXirSourceRequest *request) {
    size_t calls[2]={0};
    for(unsigned kind=0;kind<2;++kind){
        for(size_t pass=0;pass==0||pass<=calls[kind]*2;++pass){
            PublicationScope scope;XrXirSourceRequest copy={0};
            CHECK(publication_scope_begin(&scope,publication_caps)==XR_XIR_OK);
            CHECK(publication_scope_source(&scope,request,&copy)==XR_XIR_OK);
            bool oom=pass&&((pass-1)%2==0);
            publication_path_injection(kind,pass?(pass-1)/2:SIZE_MAX,oom?ERROR_NOT_ENOUGH_MEMORY:ERROR_ACCESS_DENIED);
            XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
            XrXirStatus status=xr_xir_compile_source_check(&copy,&result,&diagnostic,NULL);
            if(!pass){CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);calls[kind]=module_os_attempts[kind];}
            else{CHECK(status==(oom?XR_XIR_OUT_OF_MEMORY:XR_XIR_IO)&&diagnostic.status==status);CHECK(!result.checked&&!result.snapshot);}
            publication_path_injection_end();xr_xir_compile_source_result_free(&result);
            publication_scope_end(&scope);CHECK(!module_live&&!module_bytes);
        }
    }
    printf("Source production Windows path calls stat=%zu/fullpath=%zu exact IO/OOM physical zero\n",calls[0],calls[1]);
}
static void publication_stdlib_path_faults(const char *root, const char *specifier) {
    size_t calls[2] = {0};
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (size_t pass = 0; pass == 0 || pass <= calls[kind] * 2; ++pass) {
            XrModuleResolverConfig config = {root, NULL, NULL};
            XrModuleResolver *resolver = publication_resolver(&config); CHECK(resolver);
            XrModuleId id = {0}; char *message = NULL;
            bool oom = pass && ((pass - 1) % 2 == 0);
            publication_path_injection(kind, pass ? (pass - 1) / 2 : SIZE_MAX,
                oom ? ERROR_OUTOFMEMORY : ERROR_ACCESS_DENIED);
            XrModuleStatus status = xr_compile_module_resolver_resolve(resolver, specifier, NULL, NULL, &id, &message);
            publication_path_injection_end();
            if (!pass) { CHECK(status == XR_MODULE_OK); calls[kind] = module_os_attempts[kind]; }
            else {
                CHECK(status == (oom ? XR_MODULE_OUT_OF_MEMORY : XR_MODULE_IO));
                CHECK(!id.canonical && !id.source_path && !id.logical_path && !id.authority.physical_root);
            }
            xr_compile_module_id_cleanup(&id); publication_path_owned_free(message); xr_compile_module_resolver_free(resolver);
            CHECK(!module_live && !module_bytes);
        }
    }
    printf("stdlib %s production Windows stat=%zu/fullpath=%zu exact IO/OOM physical zero\n", specifier, calls[0], calls[1]);
}
static void publication_file_probe_cases(const char *root, const char *entry, const char *source) {
    publication_stdlib_path_faults(root, "std/io/output");
    publication_stdlib_path_faults(root, "io");
    CHECK(xr_file_probe_owned(publication_current_path_policy(), NULL, true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), "", true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), "\xC0\x80", true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), root, true) == XR_OS_IO_BAD_ARGUMENT);
    char long_path[33000]; memset(long_path, 'a', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = 0;
    CHECK(xr_file_probe_owned(publication_current_path_policy(), long_path, true) == XR_OS_IO_BUDGET);
    publication_path_injection(0, SIZE_MAX, 0);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, true) == XR_OS_IO_OK);
    CHECK(module_attempts == 1 && module_os_attempts[0] == 1);
    publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    /* Fixed API attributes cover the existing source-link/archive distinction
     * without requiring Windows symlink-creation privileges in the test host. */
    publication_path_injection(0, SIZE_MAX, 0); module_attributes_override = true;
    module_attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, true) == XR_OS_IO_OK);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, false) == XR_OS_IO_BAD_ARGUMENT);
    module_attributes |= FILE_ATTRIBUTE_DIRECTORY;
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, false) == XR_OS_IO_BAD_ARGUMENT);
    module_attributes_override = false; publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    publication_path_injection(0, SIZE_MAX, 0); module_fail_at = 0;
    CHECK(xr_file_probe_owned(publication_current_path_policy(), source, true) == XR_OS_IO_OUT_OF_MEMORY);
    CHECK(module_attempts == 1 && !module_os_attempts[0]);
    publication_path_injection_end(); CHECK(!module_live && !module_bytes);
    for (size_t error = 0; error < sizeof(path_errors) / sizeof(*path_errors); ++error) {
        publication_path_injection(0, 0, path_errors[error].error);
        CHECK(xr_file_probe_owned(publication_current_path_policy(), source, true) == path_errors[error].path);
        CHECK(module_attempts == 1 && module_os_attempts[0] == 1);
        publication_path_injection_end(); CHECK(!module_live && !module_bytes);
        for (size_t point = 0; point < 2; ++point) {
            publication_path_injection(1, point, path_errors[error].error);
            char *canonical=NULL;
            XrOsIoStatus status=xr_realpath_owned(publication_current_path_policy(),source,&canonical);
            CHECK(!canonical && status == path_errors[error].path);
            CHECK(module_os_attempts[1] == point + 1 && module_attempts == point + 1);
            publication_path_injection_end(); CHECK(!module_live && !module_bytes);
        }
    }
    char first[2048], directory[2048], second[2048];
    CHECK(snprintf(first, sizeof(first), "%s/path-probe.xr", root) > 0);
    CHECK(snprintf(directory, sizeof(directory), "%s/path-probe", root) > 0);
    CHECK(snprintf(second, sizeof(second), "%s/path-probe/index.xr", root) > 0);
    XrOsIoPolicy host_policy=xr_compile_io_policy(publication_context->resources); wchar_t *wide=NULL;CHECK(xr_win_utf8_path_owned(&host_policy,directory,&wide)==XR_OS_IO_OK&&wide);
    CHECK(CreateDirectoryW(wide, NULL)); xr_compile_resources_free(wide);
    FILE *file = fopen(first, "wb"); CHECK(file && !fclose(file));
    file = fopen(second, "wb"); CHECK(file && !fclose(file));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrModuleResolverConfig config = {root, NULL, NULL};
    for (size_t error = 0; error < sizeof(path_errors) / sizeof(*path_errors); ++error) {
        XrModuleResolver *resolver = publication_resolver(&config); CHECK(resolver);
        XrModuleId id = {0}; char *message = NULL;
        publication_path_injection(0, 0, path_errors[error].error);
        XrModuleStatus status = xr_compile_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message);
        publication_path_injection_end();
        if (path_errors[error].path == XR_OS_IO_NOT_FOUND) {
            CHECK(status == XR_MODULE_OK && module_os_attempts[0] == 2);
            CHECK(id.logical_path && strstr(id.logical_path, "path-probe/index.xr"));
        } else {
            CHECK(status == path_errors[error].module && module_os_attempts[0] == 1);
            CHECK(!id.canonical && !id.source_path && !id.logical_path && !id.authority.physical_root);
        }
        xr_compile_module_id_cleanup(&id); publication_path_owned_free(message); xr_compile_module_resolver_free(resolver);
        CHECK(!module_live && !module_bytes);
    }
    /* Every instrumented relative-resolution allocation, including UTF conversion and the
     * successful cache publication, must retain OOM rather than try index.xr. */
    size_t sites = 0;
    for (size_t pass = 0; pass == 0 || pass <= sites; ++pass) {
        XrModuleResolver *resolver = publication_resolver(&config); CHECK(resolver);
        XrModuleId id = {0}; char *message = NULL;
        publication_path_injection(0, SIZE_MAX, 0); module_fail_at = pass ? pass - 1 : SIZE_MAX;
        XrModuleStatus status = xr_compile_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message);
        publication_path_injection_end();
        if (!pass) { CHECK(status == XR_MODULE_OK); sites = module_attempts; }
        else CHECK(status == XR_MODULE_OUT_OF_MEMORY && !id.canonical && !id.source_path && !id.logical_path);
        CHECK(module_os_attempts[0] <= 1);
        xr_compile_module_id_cleanup(&id); publication_path_owned_free(message); xr_compile_module_resolver_free(resolver);
        CHECK(!module_live && !module_bytes);
    }
    CHECK(!remove(first));
    wide=NULL;CHECK(xr_win_utf8_path_owned(&host_policy,first,&wide)==XR_OS_IO_OK&&wide);
    CHECK(CreateDirectoryW(wide, NULL)); xr_compile_resources_free(wide);
    {
        XrModuleResolver *directory_resolver = publication_resolver(&config); CHECK(directory_resolver);
        XrModuleId directory_id = {0}; char *directory_message = NULL;
        publication_path_injection(0, SIZE_MAX, 0);
        CHECK(xr_compile_module_resolver_resolve(directory_resolver, "./path-probe", entry, &authority,
            &directory_id, &directory_message) == XR_MODULE_INVALID);
        CHECK(module_os_attempts[0] == 1 && !directory_id.canonical && !directory_id.source_path);
        publication_path_injection_end();
        xr_compile_module_id_cleanup(&directory_id); publication_path_owned_free(directory_message);
        xr_compile_module_resolver_free(directory_resolver); CHECK(!module_live && !module_bytes);
    }
    wide=NULL;CHECK(xr_win_utf8_path_owned(&host_policy,first,&wide)==XR_OS_IO_OK&&wide); CHECK(RemoveDirectoryW(wide)); xr_compile_resources_free(wide);
    XrModuleResolver *resolver = publication_resolver(&config); CHECK(resolver);
    XrModuleId id = {0}; char *message = NULL;
    publication_path_injection(0, SIZE_MAX, 0);
    CHECK(xr_compile_module_resolver_resolve(resolver, "./path-probe", entry, &authority, &id, &message) == XR_MODULE_OK);
    CHECK(module_os_attempts[0] == 2 && strstr(id.logical_path, "path-probe/index.xr"));
    publication_path_injection_end();
    xr_compile_module_id_cleanup(&id); publication_path_owned_free(message); xr_compile_module_resolver_free(resolver);
    CHECK(!module_live && !module_bytes && !remove(second));
    wide=NULL;CHECK(xr_win_utf8_path_owned(&host_policy,directory,&wide)==XR_OS_IO_OK&&wide);
    CHECK(RemoveDirectoryW(wide)); xr_compile_resources_free(wide);
    printf("typed file probe: 9 fixed Windows statuses; 2 realpath points; relative allocations=%zu; only NOT_FOUND advances\n", sites);
}
#else
static void publication_source_path_faults(const XrXirSourceRequest *request) { (void)request; }
static void publication_file_probe_cases(const char *root, const char *entry, const char *source) {
    XrOsIoPolicy policy=xr_compile_io_policy(publication_context->resources);
    (void)entry; CHECK(xr_file_probe_owned(&policy, root, true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(&policy, source, true) == XR_OS_IO_OK);
}
#endif
