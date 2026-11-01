/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_invocation_posix.inc.c - One POSIX C driver recipe and its reports
 *
 * KEY CONCEPT:
 *   Both compiles and the link run the same discovered driver with frozen
 *   argv. cc -MD names every header actually read and the linker report names
 *   every input actually loaded; each must lie under the workspace, the SDK
 *   or a discovered toolchain root, and every declared input must appear with
 *   the digest that was pinned before any command ran.
 */
static bool invocation_file_find(XrXirInvocation *owner, const XtcXirFileFacts *facts,
    unsigned stage, XrXirInvocationFileKind kind, uint32_t *index);

/* Single-pass order: each archive precedes the archives it references. */
static const char *const invocation_posix_archives[5] = {"lib/libxray_xir_runtime_host.a",
    "lib/libxray_xir_scalar.a", "lib/libxray_xir_admission.a", "lib/libxray_xir_declarations.a",
    "lib/libxray_compile_resources.a"};
#define INVOCATION_POSIX_FLAGS 9u
static const char *const invocation_posix_flags[INVOCATION_POSIX_FLAGS] = {"-std=c11", "-O2", "-Wall",
    "-Wextra", "-Werror", "-Wno-missing-braces", "-Wno-type-limits", "-ffp-contract=off", "-DNDEBUG"};
#ifdef XR_OS_MACOS
#define INVOCATION_POSIX_TARGET 4u
#define INVOCATION_POSIX_GROUP 0u
static const char invocation_posix_report[] = "-Wl,-dependency_info,";
#else
#define INVOCATION_POSIX_TARGET 0u
#define INVOCATION_POSIX_GROUP 2u
static const char invocation_posix_report[] = "-Wl,--dependency-file=";
#endif

static bool invocation_environment(XrXirInvocation *owner, const XrProcessView *view,
    const XrXirInvocationRequest *request) {
    if (view->env_count != 2)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    return invocation_expect(owner, view->env_keys[0], "PATH") &&
        invocation_expect(owner, view->env_values[0], request->posix.path) &&
        invocation_expect(owner, view->env_keys[1], "TMPDIR") &&
        invocation_expect(owner, view->env_values[1], request->output_directory);
}
/* Darwin commands start with -arch and -isysroot naming the discovered SDK. */
static bool invocation_posix_target(XrXirInvocation *owner, const XrProcessView *view,
    const XrXirInvocationRequest *request) {
#ifdef XR_OS_MACOS
    return invocation_expect(owner, view->argv[1], "-arch") &&
        invocation_expect(owner, view->argv[2], request->posix.arch) &&
        invocation_expect(owner, view->argv[3], "-isysroot") &&
        invocation_expect(owner, view->argv[4], request->posix.sysroot);
#else
    (void)owner; (void)view; (void)request; return true;
#endif
}
static bool invocation_compile_recipe(XrXirInvocation *owner,
    const XrXirInvocationRequest *request, unsigned stage) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    const XrProcessView *view = &owner->commands[stage];
    const XrXirInvocationCompile *compile = &request->compile[stage];
    /* The words after the flags are addressed from base + 8 onward. */
    const size_t flags = 1 + INVOCATION_POSIX_TARGET, base = flags + INVOCATION_POSIX_FLAGS - 8;
    if (view->completion_policy != XR_PROC_COMPLETE_ROOT || view->image_mode != XR_PROC_IMAGES_NONE ||
        view->argc != base + 18 + stage || !invocation_posix_target(owner, view, request))
        return owner->status != XR_XIR_INVOCATION_OK ? false :
            invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    for (unsigned i = 0; i < INVOCATION_POSIX_FLAGS; ++i)
        if (!invocation_expect(owner, view->argv[flags + i], invocation_posix_flags[i])) return false;
    static const char *const directories[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        if (!invocation_io(owner, xr_path_join_owned(&owner->io, xr_xir_runtime_sdk_root(request->sdk),
            directories[i], &path))) return false;
        bool okay = invocation_join_argument(owner, view->argv[base + 8 + i], "-I", path);
        xr_compile_resources_free(path);
        if (!okay) return false;
    }
    if (!invocation_expect(owner, view->argv[base + 11], "-c") ||
        !invocation_expect(owner, view->argv[base + 12], compile->source) ||
        !invocation_expect(owner, view->argv[base + 13], "-o") ||
        !invocation_expect(owner, view->argv[base + 14], compile->object) ||
        !invocation_expect(owner, view->argv[base + 15], "-MD") ||
        !invocation_expect(owner, view->argv[base + 16], "-MF") ||
        !invocation_expect(owner, view->argv[base + 17], compile->report)) return false;
    if (stage) {
        size_t length = 0;
        if (!invocation_length(owner, owner->prefix, &length)) return false;
        char symbol[73];
        if (length > 64) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_copy(owner, symbol, owner->prefix, length) ||
            !invocation_copy(owner, symbol + length, "_program", sizeof("_program"))) return false;
        if (!invocation_join_argument(owner, view->argv[base + 18], "-DXIR_SDK_PROGRAM_SYMBOL=", symbol)) return false;
    }
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view, request);
}
static bool invocation_link_recipe(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const char *const sdk_libraries[5]) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    const XrProcessView *view = &owner->commands[2];
    const size_t base = 1 + INVOCATION_POSIX_TARGET, archives = base + 4 + INVOCATION_POSIX_GROUP / 2;
    const size_t tail = archives + 5 + INVOCATION_POSIX_GROUP / 2;
    if (view->completion_policy != XR_PROC_COMPLETE_ROOT || view->image_mode != XR_PROC_IMAGES_NONE ||
        request->library_count || view->argc != tail + 2 || !invocation_posix_target(owner, view, request))
        return owner->status != XR_XIR_INVOCATION_OK ? false :
            invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_expect(owner, view->argv[base], "-o") ||
        !invocation_expect(owner, view->argv[base + 1], request->output) ||
        !invocation_expect(owner, view->argv[base + 2], request->compile[0].object) ||
        !invocation_expect(owner, view->argv[base + 3], request->compile[1].object)) return false;
    if (INVOCATION_POSIX_GROUP && (!invocation_expect(owner, view->argv[base + 4], "-Wl,--start-group") ||
        !invocation_expect(owner, view->argv[archives + 5], "-Wl,--end-group"))) return false;
    for (unsigned i = 0; i < 5; ++i)
        if (!invocation_expect(owner, view->argv[archives + i], sdk_libraries[i])) return false;
    if (!invocation_expect(owner, view->argv[tail], "-lm") ||
        !invocation_join_argument(owner, view->argv[tail + 1], invocation_posix_report, request->link_report))
        return false;
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view, request);
}
/* 0 equal, 1 left is an ancestor, 2 right is an ancestor, 3 disjoint. Both
 * are canonical absolute paths, compared byte-for-byte. */
static int invocation_posix_relation(const char *left, const char *right) {
    size_t a = strlen(left), b = strlen(right), common = a < b ? a : b;
    if (memcmp(left, right, common)) return 3;
    if (a == b) return 0;
    const char *longer = a < b ? right : left;
    if ((common == 1 && longer[0] == '/') || longer[common] == '/') return a < b ? 1 : 2;
    return 3;
}
static bool invocation_disjoint_directories(XrXirInvocation *owner) {
    const XtcXirDirectoryFacts *facts[2] = {
        xtc_xir_file_lease_directory_facts(owner->directories[0]),
        xtc_xir_file_lease_directory_facts(owner->directories[1])};
    if (!facts[0] || !facts[1]) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    return invocation_posix_relation(facts[0]->path, facts[1]->path) == 3 ||
        invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
}
/* A POSIX driver has no side-by-side configuration file to pin. */
static bool invocation_input_configs(XrXirInvocation *owner) { (void)owner; return true; }
static bool invocation_input_fields(const XrXirInvocationRequest *request) {
    if (!request || !request->input_directory || !request->output_directory || !request->posix.path ||
        !request->posix.root_count || request->posix.root_count > XR_XIR_POSIX_ROOT_LIMIT) return false;
#ifdef XR_OS_MACOS
    if (!request->posix.arch || !request->posix.sysroot) return false;
#endif
    for (uint32_t i = 0; i < request->posix.root_count; ++i)
        if (!request->posix.roots[i] || request->posix.roots[i][0] != '/') return false;
    return true;
}
/* The include and generated SDK roots are optional; a missing root admits no
 * header, and one created later is outside every admitted root. */
static bool invocation_input_root(XrXirInvocation *owner, unsigned index, const char *path) {
    InvocationInputs *profile = &owner->inputs;
    XrXirTargetStatus status = xtc_xir_file_lease_directory_open(owner->context.resources, path,
        &profile->directories[index]);
    if (status == XR_XIR_TARGET_UNRESOLVED && (index == 2 || index == 3)) { profile->missing[index] = true; return true; }
    if (!invocation_target(owner, status)) return false;
    profile->paths[index] = xtc_xir_file_lease_directory_facts(profile->directories[index])->path;
    return true;
}
static bool invocation_inputs_derive(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    if (!invocation_input_root(owner, 0, request->input_directory)) return false;
    static const char *const sdk_parts[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        bool okay = invocation_io(owner, xr_path_join_owned(&owner->io, xr_xir_runtime_sdk_root(request->sdk),
            sdk_parts[i], &path)) && invocation_input_root(owner, i + 1, path);
        xr_compile_resources_free(path);
        if (!okay) return false;
    }
    for (unsigned i = 1; i < 3; ++i) {
        bool same = false;
        if (!invocation_same(owner, owner->commands[0].executable, owner->commands[i].executable, &same)) return false;
        if (!same) return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    }
    XtcXirFileLease *lease = NULL;
    bool okay = invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources,
        request->output_directory, &lease));
    const char *output = okay ? xtc_xir_file_lease_directory_facts(lease)->path : NULL;
    for (unsigned i = 0; okay && i < 4; ++i)
        if (owner->inputs.paths[i] && invocation_posix_relation(owner->inputs.paths[i], output) != 3)
            okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    for (uint32_t i = 0; okay && i < request->posix.root_count; ++i)
        if (invocation_posix_relation(request->posix.roots[i], output) != 3)
            okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    xtc_xir_file_lease_free(lease); return okay;
}
/* Workspace and SDK roots admit headers; toolchain roots admit headers and
 * implicit link inputs. Paths are the canonical spelling of a lease. */
static bool invocation_input_contains(XrXirInvocation *owner, const char *path) {
    XR_DCHECK(owner->running, "toolchain roots are borrowed only during run");
    bool found = false;
    for (unsigned i = 0; !found && i < 4; ++i)
        found = owner->inputs.paths[i] && invocation_posix_relation(owner->inputs.paths[i], path) == 1;
    const XrXirPosixRecipe *recipe = &owner->running->posix;
    for (uint32_t i = 0; !found && i < recipe->root_count; ++i)
        found = invocation_posix_relation(recipe->roots[i], path) == 1;
    return found || invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
static bool invocation_toolchain_contains(XrXirInvocation *owner, const char *path) {
    const XrXirPosixRecipe *recipe = &owner->running->posix;
    for (uint32_t i = 0; i < recipe->root_count; ++i)
        if (invocation_posix_relation(recipe->roots[i], path) == 1) return true;
    return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
/* Drivers may list configuration such as the Darwin SDKSettings.json before
 * the source, so every record is a candidate header until its leased bytes
 * prove it is the pinned source. */
static bool invocation_compile_inputs(XrXirInvocation *owner, const XrDependencyFacts *report,
    unsigned stage) {
    uint32_t count = xtc_dependencies_count(report);
    bool source_seen = false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {

        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || record->kind != XR_DEPENDENCY_HEADER) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        XtcXirFileLease *lease = NULL; uint32_t index = UINT32_MAX;
        bool okay = invocation_target(owner, invocation_file_open(owner, record->path, &lease)) &&
            invocation_input_contains(owner, xtc_xir_file_lease_facts(lease)->path) &&
            invocation_file_find(owner, xtc_xir_file_lease_facts(lease), stage, XR_XIR_INVOCATION_SOURCE, &index);
        if (okay && index != UINT32_MAX) source_seen = true;
        else if (okay) {
            okay = invocation_file_find(owner, xtc_xir_file_lease_facts(lease), stage, XR_XIR_INVOCATION_HEADER, &index);
            if (okay && index == UINT32_MAX)
                okay = invocation_file_add(owner, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_HEADER, &lease);
        }
        xtc_xir_file_lease_free(lease);
        if (!okay) break;
    }
    if (owner->status == XR_XIR_INVOCATION_OK && !source_seen)
        invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    return owner->status == XR_XIR_INVOCATION_OK;
}
/* Declared objects and SDK archives must each appear with their pinned
 * bytes. Every other loaded input is a SYSTEM file under a toolchain root;
 * unresolved search candidates carry no bytes and are not inputs. */
static bool invocation_link_inputs(XrXirInvocation *owner, const XrDependencyFacts *report) {
    XR_DCHECK(owner->running, "the link report is reconciled during run");
    const char *target = xtc_dependencies_target(report);
    if (!target || strcmp(target, owner->running->output))
        return invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    uint32_t count = xtc_dependencies_count(report), declared = owner->facts.file_count;
    uint8_t *seen = NULL;
    if (!invocation_resource(owner, xr_compile_resources_calloc(owner->context.resources,
        declared, 1, (void **)&seen))) return false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {

        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || (record->kind != XR_DEPENDENCY_LINK_INPUT && record->kind != XR_DEPENDENCY_LIBRARY_SEARCH)) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        if (record->kind == XR_DEPENDENCY_LIBRARY_SEARCH) continue;
        XtcXirFileLease *lease = NULL;
        if (!invocation_target(owner, invocation_file_open(owner, record->path, &lease))) break;
        const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(lease);
        bool matched = false, okay = true;
        for (uint32_t j = 0; okay && !matched && j < declared; ++j) {

            const XrXirInvocationFile *other = &owner->files[j].facts;
            if (other->kind != XR_XIR_INVOCATION_OBJECT && other->kind != XR_XIR_INVOCATION_SDK_ARCHIVE) continue;
            bool same = false;
            okay = invocation_same(owner, facts->path, other->path, &same);
            if (!okay || !same) continue;
            if (facts->length != other->length)
                okay = invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
            if (okay) okay = invocation_bytes(owner, facts->digest, other->digest, 32);
            if (okay) { seen[j] = 1; matched = true; }
        }
        uint32_t index = UINT32_MAX;
        if (okay && !matched) okay = invocation_toolchain_contains(owner, facts->path) &&
            invocation_file_find(owner, facts, XR_XIR_INVOCATION_LINK, XR_XIR_INVOCATION_SYSTEM, &index);
        if (okay && !matched && index == UINT32_MAX)
            okay = invocation_file_add(owner, XR_XIR_INVOCATION_LINK, XR_XIR_INVOCATION_SYSTEM, &lease);
        xtc_xir_file_lease_free(lease);
        if (!okay) break;
    }
    for (uint32_t j = 0; owner->status == XR_XIR_INVOCATION_OK && j < declared; ++j) {
        XrXirInvocationFileKind kind = owner->files[j].facts.kind;
        if ((kind == XR_XIR_INVOCATION_OBJECT || kind == XR_XIR_INVOCATION_SDK_ARCHIVE) && !seen[j])
            invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    xr_compile_resources_free(seen);
    return owner->status == XR_XIR_INVOCATION_OK;
}
/* Pin the driver bytes before any command runs. POSIX executables carry no
 * version resource; the version is the driver's own predefined macros. */
static bool invocation_provider_read(XrXirInvocation *owner, unsigned stage,
    XrXirInvocationProviderImageFacts *output) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    XtcXirFileLease *lease = NULL;
    bool okay = invocation_target(owner, invocation_file_open(owner, owner->commands[stage].executable, &lease));
    if (okay) {
        const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(lease);
        output->path = facts->path; output->length = facts->length;
        memcpy(output->digest, facts->digest, sizeof(output->digest));
        output->version = owner->running->posix.version;
        owner->provider.binding = owner->running->posix.provider;
        okay = invocation_file_add(owner, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_PROVIDER_EXECUTABLE, &lease);
    }
    xtc_xir_file_lease_free(lease);
    return okay;
}
