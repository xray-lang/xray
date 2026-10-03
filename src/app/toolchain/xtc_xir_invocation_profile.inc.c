/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_invocation_profile.inc.c - One derivation of controlled MSVC roots
 */
static void invocation_profile_free(InvocationProfile *profile) {
    for (unsigned i = 0; i < 12; ++i) {
        xtc_xir_file_lease_free(profile->directories[i]);
        xr_compile_resources_free(profile->owned_paths[i]);
#ifdef XR_OS_WINDOWS
        xr_compile_resources_free(profile->wide[i]);
#endif
    }
}
static bool invocation_profile_suffix(XrXirInvocation *owner, const char *path, const char *suffix, char **output) {
    size_t a = 0, b = 0;
    if (!invocation_length(owner, path, &a) || !invocation_length(owner, suffix, &b)) return false;
    if (a > SIZE_MAX - b - 1 || a + b > owner->limits.dependencies.path_bytes)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    char *text = NULL;
    bool okay = invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources, a + b + 1, (void **)&text));
    if (okay) okay = invocation_copy(owner, text, path, a) && invocation_copy(owner, text + a, suffix, b + 1);
    if (okay) *output = text; else xr_compile_resources_free(text);
    return okay;
}
static bool invocation_profile_redirects(XrXirInvocation *owner) {
    static const char *const suffixes[] = {".local", ".manifest"};
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
        char *path = NULL;
        owner->diagnostic.stage = i ? XR_XIR_INVOCATION_LINK : XR_XIR_INVOCATION_GENERATED;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        if (!invocation_profile_suffix(owner, owner->commands[i ? 2 : 0].executable, suffixes[j], &path)) return false;
        XrOsIoStatus status = xr_file_probe_owned(&owner->io, path, false);
        xr_compile_resources_free(path);
        if (status == XR_OS_IO_NOT_FOUND) continue;
        if (status == XR_OS_IO_OK || status == XR_OS_IO_BAD_ARGUMENT || status == XR_OS_IO_UNSUPPORTED)
            return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        return invocation_io(owner, status);
    }
    return true;
}
static bool invocation_profile_configs(XrXirInvocation *owner) {
    static const char expected[] =
        "<configuration>\r\n"
        "      <startup>\r\n"
        "        <requiredRuntime safemode=\"true\" imageVersion=\"v4.0.30319\"/>\r\n"
        "        <supportedRuntime version=\"v4.0.30319\"/>\r\n"
        "        <supportedRuntime version=\"v4.5\"/>\r\n"
        "      </startup>\r\n"
        "      <runtime>\r\n"
        "        <assemblyBinding xmlns=\"urn:schemas-microsoft-com:asm.v1\">\r\n"
        "          <publisherPolicy apply=\"no\" />\r\n"
        "        </assemblyBinding>\r\n"
        "      </runtime>\r\n"
        "</configuration>\r\n\r\n";
    _Static_assert(sizeof(expected) - 1 == 409, "The supported configuration is byte-exact");
    for (unsigned i = 0; i < 2; ++i) {
        unsigned stage = i ? 2 : 0;
        owner->diagnostic.stage = (XrXirInvocationStage)stage;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        char *path = NULL; void *bytes = NULL; size_t length = 0; XtcXirFileLease *lease = NULL;
        bool okay = invocation_profile_suffix(owner, owner->commands[stage].executable, ".config", &path);
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, path, &lease));
        if (okay && xtc_xir_file_lease_facts(lease)->length != sizeof(expected) - 1)
            okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_read(lease, sizeof(expected) - 1, &bytes, &length));
        for (size_t at = 0; okay && at < length; ++at) {
            okay = invocation_work(owner, 2);
            if (okay && ((const char *)bytes)[at] != expected[at])
                okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
        if (okay) okay = invocation_file_add(owner, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_PROVIDER_CONFIG, &lease);
        xr_compile_resources_free(path); xr_compile_resources_free(bytes); xtc_xir_file_lease_free(lease);
        if (!okay) return false;
    }
    return true;
}
static bool invocation_profile_fields(const XrXirInvocationRequest *request) {
    return request && request->input_directory && request->output_directory &&
        request->msvc.vc_include && request->msvc.ucrt_include && request->msvc.shared_include &&
        request->msvc.um_include && request->msvc.system_root;
}
#ifdef XR_OS_WINDOWS
static bool invocation_profile_wide(XrXirInvocation *owner, const char *path,
    wchar_t **output, size_t *length) {
    wchar_t *wide = NULL;
    if (!invocation_io(owner, xr_win_utf8_path_owned(&owner->io, path, &wide))) return false;
    size_t count = 0;
    for (;;) {
        if (!invocation_work(owner, sizeof(wchar_t))) break;
        wchar_t c = wide[count];
        if (!c) { *output = wide; *length = count; return true; }
        if (c == L'/') {
            if (!invocation_work(owner, sizeof(wchar_t))) break;
            wide[count] = L'\\';
        }
        ++count;
    }
    xr_compile_resources_free(wide); return false;
}
/* relation is equal, left ancestor, right ancestor, or disjoint. Both inputs
 * are already admitted absolute local paths, with normalized separators. */
static bool invocation_profile_relation(XrXirInvocation *owner, const wchar_t *left, size_t a,
    const wchar_t *right, size_t b, int *relation) {
    size_t common = a < b ? a : b;
    if (!common || common > INT_MAX)
        return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_work(owner, 2 * common * sizeof(wchar_t) + 1)) return false;
    int order = CompareStringOrdinal(left, (int)common, right, (int)common, TRUE);
    if (!order) return invocation_io(owner, io_windows_status(GetLastError()));
    *relation = 3;
    if (order != CSTR_EQUAL) return true;
    if (a == b) { *relation = 0; return true; }
    const wchar_t *longer = a < b ? right : left;
    if (!invocation_work(owner, 2 * sizeof(wchar_t))) return false;
    if (longer[common - 1] == L'\\' || longer[common] == L'\\') *relation = a < b ? 1 : 2;
    return true;
}
static bool invocation_profile_equal(XrXirInvocation *owner, const char *left, const char *right, bool *same) {
    wchar_t *a = NULL, *b = NULL; size_t an = 0, bn = 0; int relation = 3;
    bool okay = invocation_profile_wide(owner, left, &a, &an) && invocation_profile_wide(owner, right, &b, &bn) &&
        invocation_profile_relation(owner, a, an, b, bn, &relation);
    xr_compile_resources_free(a); xr_compile_resources_free(b);
    if (okay) *same = relation == 0;
    return okay;
}
static bool invocation_profile_root(XrXirInvocation *owner, unsigned index, const char *path) {
    InvocationProfile *profile = &owner->profile;
    XrXirTargetStatus status = xtc_xir_file_lease_directory_open(owner->context.resources, path,
        &profile->directories[index]);
    if (status == XR_XIR_TARGET_UNRESOLVED && (index == 2 || index == 3 || index == 10)) {
        char *parent = NULL;
        bool okay = invocation_io(owner, xr_path_dirname_owned(&owner->io, path, &parent));
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources,
            parent, &profile->directories[index]));
        xr_compile_resources_free(parent);
        if (!okay) return false;
        const char *basename = index == 2 ? "include" : index == 3 ? "generated" : "System";
        const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(profile->directories[index]);
        if (!invocation_io(owner, xr_path_join_owned(&owner->io, facts->path, basename,
            &profile->owned_paths[index]))) return false;
        profile->paths[index] = profile->owned_paths[index];
        profile->missing[index] = true;
    } else {
        if (!invocation_target(owner, status)) return false;
        profile->paths[index] = xtc_xir_file_lease_directory_facts(profile->directories[index])->path;
    }
    return invocation_profile_wide(owner, profile->paths[index], &profile->wide[index], &profile->lengths[index]);
}
static bool invocation_profile_join_root(XrXirInvocation *owner, unsigned index, const char *parent, const char *name) {
    char *path = NULL;
    bool okay = invocation_io(owner, xr_path_join_owned(&owner->io, parent, name, &path));
    if (okay) okay = invocation_profile_root(owner, index, path);
    xr_compile_resources_free(path); return okay;
}
static bool invocation_profile_system_path(XrXirInvocation *owner, bool system, char **output) {
    if (!invocation_work(owner, 1)) return false;
    UINT capacity = system ? GetSystemDirectoryW(NULL, 0) : GetWindowsDirectoryW(NULL, 0);
    if (!capacity) return invocation_io(owner, io_windows_status(GetLastError()));
    if (capacity > 32768) return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    wchar_t *wide = NULL;
    if (!invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources,
        (size_t)capacity * sizeof(wchar_t), (void **)&wide))) return false;
    bool okay = invocation_work(owner, 1 + (uint64_t)capacity * sizeof(wchar_t));
    if (okay) {
        UINT count = system ? GetSystemDirectoryW(wide, capacity) : GetWindowsDirectoryW(wide, capacity);
        if (!count) okay = invocation_io(owner, io_windows_status(GetLastError()));
        else if (count >= capacity) okay = invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    }
    if (okay) okay = invocation_io(owner, xr_win_utf16_text_owned(&owner->io, wide, output));
    xr_compile_resources_free(wide); return okay;
}
static bool invocation_profile_host(XrXirInvocation *owner) {
    if (!invocation_work(owner, 1)) return false;
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetDllDirectoryW(0, NULL), error = GetLastError();
    if (!length && error) return invocation_io(owner, io_windows_status(error));
    if (length) {
        if (length > 32768) return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
        wchar_t *directory = NULL;
        if (!invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources,
            (size_t)length * sizeof(wchar_t), (void **)&directory))) return false;
        bool okay = invocation_work(owner, 1 + (uint64_t)length * sizeof(wchar_t));
        if (okay) {
            SetLastError(ERROR_SUCCESS);
            DWORD actual = GetDllDirectoryW(length, directory); error = GetLastError();
            if (!actual && error) okay = invocation_io(owner, io_windows_status(error));
            else if (actual >= length) okay = invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
            else if (actual) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
        xr_compile_resources_free(directory);
        if (!okay) return false;
    }
    if (!invocation_work(owner, 1)) return false;
    UINT32 capacity = 0;
    LONG package = GetCurrentPackageFullName(&capacity, NULL);
    if (package == APPMODEL_ERROR_NO_PACKAGE) return true;
    if (package == ERROR_SUCCESS || package == ERROR_INSUFFICIENT_BUFFER)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    return invocation_io(owner, io_windows_status((DWORD)package));
}
static bool invocation_profile_disjoint(XrXirInvocation *owner, const char *output) {
    XtcXirFileLease *lease = NULL; wchar_t *wide = NULL; size_t length = 0;
    bool okay = invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources, output, &lease));
    if (okay) okay = invocation_profile_wide(owner, xtc_xir_file_lease_directory_facts(lease)->path, &wide, &length);
    for (unsigned i = 0; okay && i < 12; ++i) {
        int relation = 0;
        okay = invocation_profile_relation(owner, owner->profile.wide[i], owner->profile.lengths[i], wide, length, &relation);
        if (okay && relation != 3) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        if (okay && owner->profile.missing[i]) {
            wchar_t *parent = NULL; size_t parent_length = 0;
            okay = invocation_profile_wide(owner,
                xtc_xir_file_lease_directory_facts(owner->profile.directories[i])->path, &parent, &parent_length);
            if (okay) okay = invocation_profile_relation(owner, parent, parent_length, wide, length, &relation);
            xr_compile_resources_free(parent);
            if (okay && relation != 3) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
    }
    xr_compile_resources_free(wide); xtc_xir_file_lease_free(lease); return okay;
}
static bool invocation_profile_derive(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    if (!invocation_profile_host(owner) || !invocation_profile_root(owner, 0, request->input_directory)) return false;
    static const char *const sdk_parts[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i)
        if (!invocation_profile_join_root(owner, i + 1, xr_xir_runtime_sdk_root(request->sdk), sdk_parts[i])) return false;
    const char *external[] = {request->msvc.vc_include, request->msvc.ucrt_include,
        request->msvc.shared_include, request->msvc.um_include};
    for (unsigned i = 0; i < 4; ++i) if (!invocation_profile_root(owner, i + 4, external[i])) return false;
    char *bin = NULL, *link_bin = NULL, *windows = NULL, *system = NULL;
    bool same = false;
    bool okay = invocation_profile_equal(owner, owner->commands[0].executable, owner->commands[1].executable, &same);
    if (okay && !same) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    if (okay) okay = invocation_io(owner, xr_path_dirname_owned(&owner->io, owner->commands[0].executable, &bin)) &&
        invocation_io(owner, xr_path_dirname_owned(&owner->io, owner->commands[2].executable, &link_bin)) &&
        invocation_profile_root(owner, 8, bin) && invocation_profile_equal(owner, bin, link_bin, &same);
    if (okay && !same) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    if (okay) okay = invocation_profile_system_path(owner, false, &windows) && invocation_profile_system_path(owner, true, &system) &&
        invocation_profile_root(owner, 9, system) && invocation_profile_join_root(owner, 10, windows, "System") &&
        invocation_profile_root(owner, 11, windows) && invocation_profile_equal(owner, windows, request->msvc.system_root, &same);
    if (okay && !same) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    if (okay) okay = invocation_profile_disjoint(owner, request->output_directory);
    xr_compile_resources_free(bin); xr_compile_resources_free(link_bin);
    xr_compile_resources_free(windows); xr_compile_resources_free(system);
    return okay;
}
static bool invocation_profile_contains(XrXirInvocation *owner, const char *path, bool image) {
    wchar_t *wide = NULL; size_t length = 0;
    if (!invocation_profile_wide(owner, path, &wide, &length)) return false;
    bool found = false;
    for (unsigned i = image ? 8 : 0; !found && i < (image ? 12u : 8u); ++i) {
        int relation = 3;
        if (!invocation_profile_relation(owner, owner->profile.wide[i], owner->profile.lengths[i], wide, length, &relation)) break;
        if (relation != 1) continue;
        found = true;
        if (i >= 9) {
            size_t at = owner->profile.lengths[i];
            if (!invocation_work(owner, sizeof(wchar_t))) break;
            if (wide[at] == L'\\') ++at;
            for (; at < length; ++at) {
                if (!invocation_work(owner, sizeof(wchar_t))) break;
                if (wide[at] == L'\\') { found = false; break; }
            }
        }
    }
    xr_compile_resources_free(wide);
    return owner->status == XR_XIR_INVOCATION_OK && (found ||
        invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0));
}
#else
static bool invocation_profile_derive(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    (void)request; return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
static bool invocation_profile_contains(XrXirInvocation *owner, const char *path, bool image) {
    (void)path; (void)image; return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
#endif
