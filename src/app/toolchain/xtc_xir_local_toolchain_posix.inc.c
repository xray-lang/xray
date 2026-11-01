/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_local_toolchain_posix.inc.c - One C driver and its observed facts
 *
 * KEY CONCEPT:
 *   macOS asks xcrun for the selected SDK (and clang when none is named);
 *   Linux defaults to /usr/bin/cc. One preprocessor probe under the exact
 *   build environment then proves the driver targets this host and reports
 *   its version and include search list; -print-search-dirs adds the library
 *   roots. Only canonical paths and these facts are retained.
 */
#define LOCAL_PROBE_MACROS 14u

/* POSIX discovery uses TMPDIR, then /tmp, for the private workspace parent. */
static bool local_workspace(XtcXirLocalToolchain *owner, const char *explicit_parent) {
    if (explicit_parent) return local_path(owner, explicit_parent, true, &owner->view.workspace_parent);
    char *candidate = NULL;
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "TMPDIR", &candidate);
    if (status != XR_OS_IO_NOT_FOUND && !local_io(owner, status)) return false;
    bool okay = local_path(owner, candidate && candidate[0] ? candidate : "/tmp", true,
        &owner->view.workspace_parent);
    xr_compile_resources_free(candidate); return okay;
}
#ifdef XR_OS_MACOS
/* One output line without its newline; other control bytes are rejected. */
static bool local_line(XtcXirLocalToolchain *owner, const XrProcessByteBuffer *bytes, char **output) {
    if (!bytes->data || bytes->truncated) return local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_SELF, 0);
    size_t end = bytes->length;
    while (end && (bytes->data[end - 1] == '\n' || bytes->data[end - 1] == '\r')) --end;
    if (!end) return local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_SELF, 0);
    for (size_t i = 0; i < end; ++i) {

        if (bytes->data[i] < 32) return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
    }
    *output = local_copy(owner, (const char *)bytes->data, end); return *output != NULL;
}
static bool local_xcrun(XtcXirLocalToolchain *owner, const XtcXirLocalToolchainRequest *request,
    const char *option, const char *argument, char **output) {
    XrProcessSpec spec; XrProcessResult result = {0};
    xtc_process_spec_init(&spec, "/usr/bin/xcrun", request->timeout_ms);
    spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT; spec.cwd = "/";
    spec.output_limit = request->output_limit;
    spec.argv[1] = option; spec.argv[2] = argument;
    bool okay = local_process(owner, &spec, &result) && local_line(owner, &result.stdout_bytes, output);
    xtc_process_result_free(&result); return okay;
}
#endif
/* Resolve a candidate that may not exist. Absent paths, and paths that do not
 * name the requested kind, yield NULL without a failure; links are followed. */
static bool local_existing(XtcXirLocalToolchain *owner, const char *path, bool directory, char **output) {
    char *resolved = NULL;
    XrOsIoStatus status = xr_realpath_owned(&owner->policy, path, &resolved);
    if (status == XR_OS_IO_NOT_FOUND) return true;
    if (!local_io(owner, status)) return false;
    XrFsStat stat = {0};
    status = xr_os_io_stat(&owner->policy, resolved, &stat);
    if (status == XR_OS_IO_OK && stat.kind == (directory ? XR_FS_DIR : XR_FS_FILE)) { *output = resolved; return true; }
    xr_compile_resources_free(resolved);
    return status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND || local_io(owner, status);
}
/* A name without a slash is searched in the absolute PATH entries only. */
static bool local_search(XtcXirLocalToolchain *owner, const char *name) {
    char *search = NULL, *candidate = NULL, *resolved = NULL;
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "PATH", &search);
    if (status == XR_OS_IO_NOT_FOUND) return local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_FILESYSTEM, status);
    bool okay = local_io(owner, status), found = false;
    for (size_t at = 0; okay && !found && search[at];) {
        size_t end = at;
        while (search[end] && search[end] != ':') ++end;
        char saved = search[end]; search[end] = 0;
        if (search[at] == '/') {
            okay = local_io(owner, xr_path_join_owned(&owner->policy, search + at, name, &candidate)) &&
                local_existing(owner, candidate, false, &resolved);
            if (okay && resolved) { found = true; okay = local_path(owner, resolved, false, &owner->view.compiler); }
            xr_compile_resources_free(candidate); candidate = NULL;
            xr_compile_resources_free(resolved); resolved = NULL;
        }
        search[end] = saved; at = saved ? end + 1 : end;
    }
    xr_compile_resources_free(search);
    if (okay && !found) okay = local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_FILESYSTEM, XR_OS_IO_NOT_FOUND);
    return okay;
}
static bool local_compiler(XtcXirLocalToolchain *owner, const char *name) {
    for (const char *c = name; *c; ++c)
        if (*c == '/') return local_path(owner, name, false, &owner->view.compiler);
    return name[0] ? local_search(owner, name) : local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
}
/* Commands see the driver's own directory, then the system tool directories. */
static bool local_command_path(XtcXirLocalToolchain *owner) {
    char *directory = NULL; size_t length = 0;
    if (!local_io(owner, xr_path_dirname_owned(&owner->policy, owner->view.compiler, &directory))) return false;
    bool okay = local_length(owner, directory, &length);
    static const char suffix[] = ":/usr/bin:/bin";
    char *path = okay ? local_allocate(owner, length + sizeof(suffix)) : NULL;
    if (path) { memcpy(path, directory, length); memcpy(path + length, suffix, sizeof(suffix)); }
    xr_compile_resources_free(directory);
    return path && local_keep(owner, path, &owner->view.posix.path);
}
static void local_probe_spec(XtcXirLocalToolchain *owner, const XtcXirLocalToolchainRequest *request,
    XrProcessSpec *spec, size_t *argc) {
    XR_DCHECK(owner->view.compiler && owner->view.posix.path, "the probe runs the resolved driver");
    xtc_process_spec_init(spec, owner->view.compiler, request->timeout_ms);
    spec->environment_source = XTC_PROCESS_ENV_EXPLICIT; spec->cwd = "/";
    spec->output_limit = request->output_limit; spec->env_count = 2;
    spec->env_keys[0] = "PATH"; spec->env_values[0] = owner->view.posix.path;
    spec->env_keys[1] = "TMPDIR"; spec->env_values[1] = owner->view.workspace_parent;
    *argc = 1;
    if (owner->view.posix.arch) { spec->argv[(*argc)++] = "-arch"; spec->argv[(*argc)++] = owner->view.posix.arch; }
    if (owner->view.posix.sysroot) {
        spec->argv[(*argc)++] = "-isysroot"; spec->argv[(*argc)++] = owner->view.posix.sysroot;
    }
}
/* Root candidates that do not exist are skipped; duplicates are kept once. */
static bool local_root(XtcXirLocalToolchain *owner, const char *path) {
    char *resolved = NULL, *canonical = NULL;
    if (!local_existing(owner, path, true, &resolved)) return false;
    if (!resolved) return true;
    bool okay = local_path_receive(owner, resolved, true, &canonical);
    xr_compile_resources_free(resolved);
    if (!okay) return false;
    XrXirPosixRecipe *recipe = &owner->view.posix;
    for (uint32_t i = 0; i < recipe->root_count; ++i) {
        if (!strcmp(recipe->roots[i], canonical)) { xr_compile_resources_free(canonical); return true; }
    }
    if (recipe->root_count == XR_XIR_POSIX_ROOT_LIMIT) {
        xr_compile_resources_free(canonical);
        return local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_SELF, 0);
    }
    if (!local_keep(owner, canonical, &recipe->roots[recipe->root_count])) return false;
    ++recipe->root_count; return true;
}
/* Mutable copy of process output split into NUL-terminated lines. */
static char *local_text(XtcXirLocalToolchain *owner, const XrProcessByteBuffer *bytes) {
    if (!bytes->data || bytes->truncated) {
        local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_PROCESS, 0); return NULL;
    }
    char *text = local_copy(owner, (const char *)bytes->data, bytes->length);
    for (size_t i = 0; text && i < bytes->length; ++i) {

        if (text[i] == '\n' || text[i] == '\r') text[i] = 0;
        else if (!text[i]) {
            xr_compile_resources_free(text);
            local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0); return NULL;
        }
    }
    return text;
}
static bool local_number(const char *text, uint16_t *output) {
    uint32_t value = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || (value = value * 10 + (uint32_t)(*text - '0')) > 65535) return false;
    }
    *output = (uint16_t)value; return true;
}
static bool local_version(XtcXirLocalToolchain *owner, const char *const *parts) {
    XtcXirPeVersion *version = &owner->view.posix.version;
    for (unsigned i = 0; i < 3; ++i) {
        if (!local_number(parts[i], &version->file[i]))
            return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
        version->product[i] = version->file[i];
    }
    int written = snprintf(version->file_text, sizeof(version->file_text), "%u.%u.%u",
        (unsigned)version->file[0], (unsigned)version->file[1], (unsigned)version->file[2]);
    if (written <= 0 || (size_t)written >= sizeof(version->file_text))
        return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
    memcpy(version->product_text, version->file_text, sizeof(version->file_text));
    return true;
}
static bool local_equal(const char *actual, const char *expected) { return actual && !strcmp(actual, expected); }
/* The driver must target this host's CPU, its object format (Mach-O or ELF)
 * and the LP64 little-endian model, in strict C11. */
static bool local_macros(XtcXirLocalToolchain *owner, const char *text, size_t length) {
    static const char *const names[LOCAL_PROBE_MACROS] = {"__clang__", "__clang_major__", "__clang_minor__",
        "__clang_patchlevel__", "__GNUC__", "__GNUC_MINOR__", "__GNUC_PATCHLEVEL__", "__STDC_VERSION__",
        "__SIZEOF_POINTER__", "__BYTE_ORDER__", "__x86_64__", "__aarch64__", "__MACH__", "__ELF__"};
    const char *values[LOCAL_PROBE_MACROS] = {0};
    for (size_t at = 0; at < length; at += strlen(text + at) + 1) {
        const char *line = text + at;
        if (strncmp(line, "#define ", 8)) continue;
        const char *name = line + 8, *space = strchr(name, ' ');
        if (!space) continue;
        for (unsigned i = 0; i < LOCAL_PROBE_MACROS; ++i) {
            if (strlen(names[i]) != (size_t)(space - name) || strncmp(names[i], name, (size_t)(space - name))) continue;
            if (values[i]) return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
            values[i] = space + 1;
        }
    }
#ifdef XR_ARCH_X86_64
    bool arch = local_equal(values[10], "1") && !values[11];
#else
    bool arch = local_equal(values[11], "1") && !values[10];
#endif
#ifdef XR_OS_MACOS
    bool system = local_equal(values[12], "1") && !values[13];
#else
    bool system = local_equal(values[13], "1") && !values[12];
#endif
    if (!arch || !system || !local_equal(values[7], "201112L") || !local_equal(values[8], "8") ||
        !local_equal(values[9], "__ORDER_LITTLE_ENDIAN__"))
        return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
    bool clang = local_equal(values[0], "1");
    if (!clang && !values[4]) return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
    owner->view.posix.provider = clang ? XR_TOOLCHAIN_BINDING_PROVIDER_CLANG : XR_TOOLCHAIN_BINDING_PROVIDER_GCC;
    return local_version(owner, clang ? values + 1 : values + 4);
}
/* The "#include <...>" block of -v output; Darwin marks framework roots. */
static bool local_includes(XtcXirLocalToolchain *owner, char *text, size_t length) {
    static const char framework[] = " (framework directory)";
    bool inside = false, closed = false;
    for (size_t at = 0, size = 0; owner->diagnostic.status == XTC_XIR_LOCAL_OK && at < length; at += size + 1) {
        char *line = text + at;
        size = strlen(line);
        if (!inside) { inside = !strcmp(line, "#include <...> search starts here:"); continue; }
        if (!strcmp(line, "End of search list.")) { closed = true; break; }
        if (line[0] != ' ' || line[1] != '/') return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
        size_t suffix = sizeof(framework) - 1;
        if (size > suffix && !strcmp(line + size - suffix, framework)) line[size - suffix] = 0;
        if (!local_root(owner, line + 1)) return false;
    }
    return owner->diagnostic.status == XTC_XIR_LOCAL_OK &&
        (closed || local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0));
}
static bool local_libraries(XtcXirLocalToolchain *owner, char *text, size_t length) {
    static const char prefix[] = "libraries: =";
    for (size_t at = 0; at < length; at += strlen(text + at) + 1) {
        char *line = text + at;
        if (strncmp(line, prefix, sizeof(prefix) - 1)) continue;
        for (char *entry = line + sizeof(prefix) - 1; *entry;) {
            char *end = entry;
            while (*end && *end != ':') ++end;
            char saved = *end; *end = 0;
            if (entry[0] == '/' && !local_root(owner, entry)) return false;
            if (!saved) break;
            entry = end + 1;
        }
        return true;
    }
    return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
}
static bool local_probe(XtcXirLocalToolchain *owner, const XtcXirLocalToolchainRequest *request) {
    static const char *const facts[] = {"-std=c11", "-E", "-dM", "-v", "-x", "c", "/dev/null"};
    XrProcessSpec spec; size_t argc = 0; XrProcessResult result = {0};
    local_probe_spec(owner, request, &spec, &argc);
    for (unsigned i = 0; i < sizeof(facts) / sizeof(facts[0]); ++i) spec.argv[argc++] = facts[i];
    bool okay = local_process(owner, &spec, &result);
    char *macros = okay ? local_text(owner, &result.stdout_bytes) : NULL;
    char *search = macros ? local_text(owner, &result.stderr_bytes) : NULL;
    okay = search && local_macros(owner, macros, result.stdout_bytes.length) &&
        local_includes(owner, search, result.stderr_bytes.length);
    xr_compile_resources_free(macros); xr_compile_resources_free(search); xtc_process_result_free(&result);
    if (!okay) return false;
    local_probe_spec(owner, request, &spec, &argc);
    spec.argv[argc] = "-print-search-dirs";
    okay = local_process(owner, &spec, &result);
    char *libraries = okay ? local_text(owner, &result.stdout_bytes) : NULL;
    okay = libraries && local_libraries(owner, libraries, result.stdout_bytes.length);
    xr_compile_resources_free(libraries); xtc_process_result_free(&result);
    return okay;
}
static bool local_discover(XtcXirLocalToolchain *owner, const XtcXirLocalToolchainRequest *request) {
    const char *selected = request->compiler;
    bool okay = true;
#ifdef XR_OS_MACOS
    char *sdk = NULL, *clang = NULL;
    okay = local_xcrun(owner, request, "--show-sdk-path", NULL, &sdk) &&
        local_path(owner, sdk, true, &owner->view.posix.sysroot) &&
        (selected || local_xcrun(owner, request, "--find", "clang", &clang));
    if (okay && !selected) selected = clang;
    owner->view.posix.arch = XR_XIR_SDK_POSIX_ARCH_NAME;
#else
    if (!selected) selected = "/usr/bin/cc";
#endif
    if (okay) owner->diagnostic.stage = XTC_XIR_LOCAL_PATHS;
    if (okay) okay = local_compiler(owner, selected) && local_command_path(owner);
#ifdef XR_OS_MACOS
    xr_compile_resources_free(sdk); xr_compile_resources_free(clang);
#endif
    if (okay) owner->diagnostic.stage = XTC_XIR_LOCAL_ENVIRONMENT;
    if (okay && owner->view.posix.sysroot) okay = local_root(owner, owner->view.posix.sysroot);
    if (okay) okay = local_probe(owner, request);
    if (okay) { owner->view.linker = owner->view.compiler; owner->diagnostic.stage = XTC_XIR_LOCAL_PATHS; }
    return okay;
}
