/* Windows discovery retains only the four requested environment fields. */
static bool local_workspace(XtcXirLocalToolchain *owner, const char *explicit_parent) {
    if (explicit_parent) return local_path(owner, explicit_parent, true, &owner->view.workspace_parent);
    const char *keys[2] = {"TEMP", "TMP"}; char *candidate = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, keys[i], &candidate);
        if (status == XR_OS_IO_NOT_FOUND) continue;
        if (!local_io(owner, status)) return false;
        if (!local_work(owner, 1)) { xr_compile_resources_free(candidate); return false; }
        bool nonempty = candidate[0] != 0;
        if (nonempty) {
            bool okay = local_path(owner, candidate, true, &owner->view.workspace_parent);
            xr_compile_resources_free(candidate); return okay;
        }
        xr_compile_resources_free(candidate); candidate = NULL;
    }
    const DWORD capacity = 32768;
    wchar_t *wide = local_allocate(owner, (size_t)capacity * sizeof(wchar_t));
    if (!wide) return false;
    bool okay = local_work(owner, 1 + (uint64_t)capacity * sizeof(wchar_t));
    if (okay) {
        DWORD count = GetTempPathW(capacity, wide);
        if (!count) {
            DWORD error = GetLastError();
            okay = local_io(owner, io_windows_status(error)); owner->diagnostic.os_error = error;
        } else if (count >= capacity) okay = local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_FILESYSTEM, XR_OS_IO_BUDGET);
        else okay = local_io(owner, xr_win_utf16_text_owned(&owner->policy, wide, &candidate));
    }
    if (okay) okay = local_path(owner, candidate, true, &owner->view.workspace_parent);
    xr_compile_resources_free(candidate); xr_compile_resources_free(wide); return okay;
}
static bool local_same_path(XtcXirLocalToolchain *owner, const char *a, const char *b) {
    wchar_t *left = NULL, *right = NULL; size_t left_size = 0, right_size = 0;
    bool okay = local_io(owner, xr_win_utf8_owned(&owner->policy, a, &left, &left_size)) &&
        local_io(owner, xr_win_utf8_owned(&owner->policy, b, &right, &right_size));
    if (okay && local_work(owner, 1 + (left_size + right_size) * sizeof(wchar_t))) {
        int compared = CompareStringOrdinal(left, (int)left_size, right, (int)right_size, TRUE);
        if (!compared) { DWORD error = GetLastError(); okay = local_io(owner, io_windows_status(error)); owner->diagnostic.os_error = error; }
        else if (compared != CSTR_EQUAL) okay = local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
    } else okay = false;
    xr_compile_resources_free(left); xr_compile_resources_free(right); return okay;
}
/* Output is UTF-16LE from cmd /u. Parsing is length-based; embedded NUL,
 * duplicate selected keys, truncated code units and invalid selected UTF-16
 * are rejected. Other environment values are neither retained nor logged. */
static bool local_environment(XtcXirLocalToolchain *owner, const XrProcessByteBuffer *bytes,
    const char **values) {
    static const wchar_t *const keys[4] = {L"VCToolsInstallDir", L"WindowsSdkDir", L"WindowsSDKVersion", L"SystemRoot"};
    static const size_t sizes[4] = {17, 13, 17, 10};
    if (!bytes || !bytes->data || !bytes->length || bytes->length % 2 || bytes->truncated)
        return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
    if (bytes->length > SIZE_MAX - sizeof(wchar_t)) return local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_SELF, 0);
    wchar_t *text = local_allocate(owner, bytes->length + sizeof(wchar_t));
    if (!text) return false;
    bool okay = local_work(owner, bytes->length + sizeof(wchar_t));
    size_t units = bytes->length / 2, at = 0; unsigned present = 0;
    if (okay) { memcpy(text, bytes->data, bytes->length); text[units] = 0; }
    if (okay && local_work(owner, 2)) { if (text[0] == 0xfeff) ++at; } else okay = false;
    while (okay && at < units) {
        size_t start = at, equals = SIZE_MAX;
        while (at < units && local_work(owner, 2)) {
            wchar_t value = text[at];
            if (!value) { okay = local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0); break; }
            if (value == L'\r' || value == L'\n') break;
            if (value == L'=' && equals == SIZE_MAX) equals = at;
            ++at;
        }
        if (owner->diagnostic.status != XTC_XIR_LOCAL_OK) okay = false;
        size_t end = at;
        while (okay && at < units && local_work(owner, 2)) {
            if (text[at] != L'\r' && text[at] != L'\n') break;
            ++at;
        }
        if (owner->diagnostic.status != XTC_XIR_LOCAL_OK) okay = false;
        if (!okay) break;
        for (unsigned key = 0; key < 4; ++key) {
            if (equals == SIZE_MAX || equals - start != sizes[key]) continue;
            bool matches = true;
            for (size_t i = 0; i < sizes[key]; ++i) {
                if (!local_work(owner, 4)) { okay = false; break; }
                wchar_t a = text[start+i], b = keys[key][i];
                if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
                if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
                if (a != b) { matches = false; break; }
            }
            if (!okay) break;
            if (!matches) continue;
            if ((present & (1u << key)) || end == equals + 1) {
                okay = local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0); break;
            }
            if (!local_work(owner, 2)) { okay = false; break; }
            text[end] = 0; char *value = NULL;
            okay = local_io(owner, xr_win_utf16_text_owned(&owner->policy, text + equals + 1, &value)) &&
                local_keep(owner, value, &values[key]);
            if (okay) present |= 1u << key;
            break;
        }
    }
    xr_compile_resources_free(text);
    if (okay && present != 15) okay = local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_SELF, 0);
    return okay;
}
static bool local_version_component(XtcXirLocalToolchain *owner, const char *version, char **output) {
    size_t length = 0;
    if (!local_length(owner, version, &length)) return false;
    while (length) {
        if (!local_work(owner, 1)) return false;
        if (version[length-1] != '/' && version[length-1] != '\\') break;
        --length;
    }
    bool digit = false;
    for (size_t i = 0; i < length; ++i) {
        if (!local_work(owner, 1)) return false;
        char c = version[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '.' || !digit || i + 1 == length)
            return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
        else digit = false;
    }
    if (!digit) return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
    *output = local_copy(owner, version, length); return *output != NULL;
}
static bool local_paths(XtcXirLocalToolchain *owner, const char *const *values, const char *compiler) {
    char *version = NULL, *include = NULL, *libraries = NULL, *joined = NULL;
    const char *vc = NULL, *kit = NULL;
    bool okay = local_path(owner, values[0], true, &vc) && local_path(owner, values[1], true, &kit) &&
        local_path(owner, values[3], true, &owner->view.msvc.system_root) &&
        local_version_component(owner, values[2], &version) &&
        local_join_path(owner, vc, "bin/Hostx64/x64/cl.exe", false, &owner->view.compiler) &&
        local_join_path(owner, vc, "bin/Hostx64/x64/link.exe", false, &owner->view.linker) &&
        local_join_path(owner, vc, "include", true, &owner->view.msvc.vc_include);
    if (okay && compiler) {
        const char *actual = NULL;
        okay = local_path(owner, compiler, false, &actual) && local_same_path(owner, actual, owner->view.compiler);
    }
    if (okay) okay = local_io(owner, xr_path_join_owned(&owner->policy, kit, "Include", &joined)) &&
        local_io(owner, xr_path_join_owned(&owner->policy, joined, version, &include));
    xr_compile_resources_free(joined); joined = NULL;
    if (okay) okay = local_io(owner, xr_path_join_owned(&owner->policy, kit, "Lib", &joined)) &&
        local_io(owner, xr_path_join_owned(&owner->policy, joined, version, &libraries));
    if (okay) okay = local_join_path(owner, include, "ucrt", true, &owner->view.msvc.ucrt_include) &&
        local_join_path(owner, include, "shared", true, &owner->view.msvc.shared_include) &&
        local_join_path(owner, include, "um", true, &owner->view.msvc.um_include);
    static const char *const names[5] = {"lib/x64/msvcrt.lib", "lib/x64/oldnames.lib", "lib/x64/vcruntime.lib",
        "ucrt/x64/ucrt.lib", "um/x64/kernel32.Lib"};
    for (unsigned i = 0; okay && i < 5; ++i) {
        okay = local_join_path(owner, i < 3 ? vc : libraries, names[i], false, &owner->view.libraries[i].path);
        if (okay) okay = local_work(owner, sizeof(owner->view.libraries[i].kind));
        if (okay) owner->view.libraries[i].kind = i == 4 ? XR_XIR_INVOCATION_SYSTEM : XR_XIR_INVOCATION_CRT;
    }
    if (okay && local_work(owner, sizeof(owner->view.library_count))) owner->view.library_count = 5;
    else okay = false;
    xr_compile_resources_free(version); xr_compile_resources_free(include);
    xr_compile_resources_free(libraries); xr_compile_resources_free(joined); return okay;
}
static bool local_installation(XtcXirLocalToolchain *owner, const XrProcessByteBuffer *bytes, char **output) {
    if (!bytes->data || !bytes->length || bytes->truncated)
        return local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_SELF, 0);
    size_t start = 0, end = bytes->length;
    if (bytes->length >= 3) {
        if (!local_work(owner, 3)) return false;
        if (bytes->data[0] == 0xef && bytes->data[1] == 0xbb && bytes->data[2] == 0xbf) start = 3;
    }
    while (end > start) {
        if (!local_work(owner, 1)) return false;
        if (bytes->data[end-1] != '\r' && bytes->data[end-1] != '\n') break;
        --end;
    }
    if (start == end) return local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_SELF, 0);
    for (size_t i = start; i < end; ++i) {
        if (!local_work(owner, 1)) return false;
        unsigned char c = bytes->data[i];
        /* cmd expands percent even inside quotes; batch metacharacters and
         * delayed expansion are not permitted in this discovery command. */
        if (!c || c == '\r' || c == '\n' || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^' || c == '%' || c == '!')
            return local_fail(owner, XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_SELF, 0);
    }
    *output = local_copy(owner, (const char *)bytes->data + start, end - start); return *output != NULL;
}
static bool local_discover(XtcXirLocalToolchain *owner, const XtcXirLocalToolchainRequest *request) {
    char *program_files = NULL, *system_root = NULL, *installation = NULL;
    const char *vswhere = NULL, *cmd = NULL, *script = NULL, *discovery_cwd = NULL, *values[4] = {0};
    XrProcessResult result = {0};
    bool okay = local_io(owner, xr_os_io_environment_get(&owner->policy, "ProgramFiles(x86)", &program_files)) &&
        local_join_path(owner, program_files, "Microsoft Visual Studio/Installer/vswhere.exe", false, &vswhere) &&
        local_io(owner, xr_os_io_environment_get(&owner->policy, "SystemRoot", &system_root)) &&
        local_path(owner, system_root, true, &discovery_cwd) &&
        local_join_path(owner, discovery_cwd, "System32/cmd.exe", false, &cmd);
    XrProcessSpec spec;
    if (okay) okay = local_work(owner, sizeof(spec));
    if (okay) {
        xtc_process_spec_init(&spec, vswhere, request->timeout_ms);
        spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT; spec.cwd = discovery_cwd;
        spec.output_limit = request->output_limit;
        spec.argv[1] = "-products"; spec.argv[2] = "*"; spec.argv[3] = "-requires";
        spec.argv[4] = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
        spec.argv[5] = "-property"; spec.argv[6] = "installationPath";
        spec.argv[7] = "-format"; spec.argv[8] = "value"; spec.argv[9] = "-latest"; spec.argv[10] = "-utf8";
        okay = local_process(owner, &spec, &result) &&
            local_installation(owner, &result.stdout_bytes, &installation) &&
            local_join_path(owner, installation, "Common7/Tools/VsDevCmd.bat", false, &script);
    }
    xtc_process_result_free(&result);
    if (okay) okay = local_work(owner, sizeof(spec));
    if (okay) {
        owner->diagnostic.stage = XTC_XIR_LOCAL_ENVIRONMENT;
        xtc_process_spec_init(&spec, cmd, request->timeout_ms);
        spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT; spec.cwd = discovery_cwd;
        spec.output_limit = request->output_limit;
        spec.argv[1] = "/d"; spec.argv[2] = "/s"; spec.argv[3] = "/u"; spec.argv[4] = "/c";
        spec.argv[5] = "call"; spec.argv[6] = script; spec.argv[7] = "-no_logo";
        spec.argv[8] = "-arch=x64"; spec.argv[9] = "-host_arch=x64"; spec.argv[10] = "&&"; spec.argv[11] = "set";
        okay = local_process(owner, &spec, &result) &&
            local_environment(owner, &result.stdout_bytes, values);
    }
    xtc_process_result_free(&result);
    if (okay) { owner->diagnostic.stage = XTC_XIR_LOCAL_PATHS; okay = local_paths(owner, values, request->compiler); }
    xr_compile_resources_free(program_files); xr_compile_resources_free(system_root);
    xr_compile_resources_free(installation); return okay;
}
