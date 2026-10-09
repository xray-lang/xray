/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_owned_string_source_resources.h - Real detached Source compiler pipeline
 *
 * KEY CONCEPT:
 *   A String result has a Domain lease independent from immutable program code.
 */
#ifndef XIR_OWNED_STRING_SOURCE_RESOURCES_H
#define XIR_OWNED_STRING_SOURCE_RESOURCES_H

#define CS_MAX_FUNCTIONS 32u
static const char cs_source[] =
    "fn identity<T>(value: T) -> T { return value; }\n"
    "export fn owned() -> string {\n"
    "    const value = identity<string>(\"root-\" + \"owned-result\");\n"
    "    print(\"C2\", 42, true, value);\n"
    "    return value;\n"
    "}\n";

typedef struct CsFixture {
    XrXirCompileContext compiler;
    XrXirArtifact *lowered;
    XrXirProgram *program;
    XrXirVmBinding bindings[CS_MAX_FUNCTIONS];
    XrXirCallEntry entries[CS_MAX_FUNCTIONS];
    uint32_t main_entry, owned_entry, module;
    unsigned code_releases;
    char root[1024], file[1060];
} CsFixture;

static void cs_code_release(void *opaque) {
    CsFixture *f = opaque;
    CHECK(f->lowered && !f->code_releases);
    ++f->code_releases;
    xr_xir_compile_artifact_free(f->lowered);
    f->lowered = NULL;
}

static void cs_compiler_step(CsMetrics *m, unsigned phase, size_t begin) {
    CHECK(phase < CS_COMPILER_PHASES && instance_compile_attempts >= begin);
    m->compiler_sites[phase] = instance_compile_attempts - begin;
}

static void cs_fixture_source(CsFixture *f) {
    int n = snprintf(f->root, sizeof(f->root), "%s", XR_OWNED_STRING_SCRATCH);
    CHECK(n > 0 && (size_t)n < sizeof(f->root));
    n = snprintf(f->file, sizeof(f->file), "%s/root.xr", f->root);
    CHECK(n > 0 && (size_t)n < sizeof(f->file));
    XrOsIoPolicy io = xr_compile_io_policy(f->compiler.resources);
    CHECK(xr_os_io_mkdir(&io, f->root, 0700) == XR_OS_IO_OK);
    bool exists = false;
    XrOsIoStatus status = xr_os_io_exists(&io, f->file, &exists);
    CHECK(status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND);
    CHECK(status != XR_OS_IO_NOT_FOUND || !exists);
    if (exists) CHECK(xr_os_io_remove(&io, f->file) == XR_OS_IO_OK);
    CHECK(xr_os_io_write_new_file_sync(&io, f->file, (const uint8_t *)cs_source,
        sizeof(cs_source) - 1) == XR_OS_IO_OK);
}

static uint32_t cs_export(const XrXirModule *m, const char *name, XrXirType result) {
    uint32_t id = UINT32_MAX;
    for (uint32_t i = 0; i < m->function_count; ++i) {
        const XrXirFunction *fn = &m->functions[i];
        const XrXirFunctionIdentity *identity = &m->declarations->functions[i];
        if (fn->name_length != strlen(name) || memcmp(fn->name, name, fn->name_length) ||
            identity->module != m->declarations->root_module || identity->nominal_owner) continue;
        CHECK(identity->exported && id == UINT32_MAX && !fn->parameter_count && fn->result == result);
        id = i;
    }
    CHECK(id != UINT32_MAX);
    return id;
}

static void cs_source_checked(CsFixture *f, CsMetrics *m, XrXirArtifact **checked) {
    XrCompilerSession *session = NULL;
    size_t begin = instance_compile_attempts;
    CHECK(xr_compile_session_new(f->compiler.resources, &session) == XR_COMPILER_SESSION_OK);
    cs_compiler_step(m, 1, begin);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, f->root};
    XrXirSourceRequest request = {session, f->file, &authority, &f->compiler,
        NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    char *failure = NULL;
    begin = instance_compile_attempts;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    if (status != XR_XIR_OK)
        fprintf(stderr, "C2 Source status=%u line=%d column=%d message=%s\n",
            status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot && !failure);
    const XrXirModule *module = xr_xir_compile_artifact_module(result.checked);
    CHECK(module->stage == XR_XIR_CHECKED && module->generics);
    unsigned templates = 0;
    for (uint32_t i = 0; i < module->function_count; ++i)
        if (module->generics[i].parameter_count) {
            CHECK(module->generics[i].parameter_count == 1);
            CHECK(module->functions[i].name_length == 8 && !memcmp(module->functions[i].name, "identity", 8));
            ++templates;
        }
    CHECK(templates == 1);
    *checked = result.checked;
    result.checked = NULL;
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    XrOsIoPolicy io = xr_compile_io_policy(f->compiler.resources);
    CHECK(xr_os_io_remove(&io, f->file) == XR_OS_IO_OK);
    cs_compiler_step(m, 2, begin);
}

static void cs_packet_roundtrip(CsFixture *f, CsMetrics *m, XrXirArtifact **checked) {
    XrXirCheckedPacket packet = {0};
    size_t begin = instance_compile_attempts;
    CHECK(xr_xir_compile_checked_write(*checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(*checked);
    *checked = NULL;
    cs_compiler_step(m, 3, begin);
    begin = instance_compile_attempts;
    CHECK(xr_xir_compile_checked_read(&f->compiler, packet.bytes, packet.length, checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_context(*checked)->resources == f->compiler.resources);
    cs_compiler_step(m, 4, begin);
}

static void cs_lower(CsFixture *f, CsMetrics *m, XrXirArtifact *checked) {
    XrXirArtifact *closed = NULL;
    size_t begin = instance_compile_attempts;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_context(closed)->resources == f->compiler.resources);
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    for (uint32_t i = 0; module->generics && i < module->function_count; ++i)
        CHECK(!module->generics[i].parameter_count);
    cs_compiler_step(m, 5, begin);
    begin = instance_compile_attempts;
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    cs_compiler_step(m, 6, begin);
    begin = instance_compile_attempts;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &f->lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_module(f->lowered)->stage == XR_XIR_LOWERED);
    CHECK(xr_xir_compile_artifact_context(f->lowered)->resources == f->compiler.resources);
    cs_compiler_step(m, 7, begin);
}

static void cs_seal(CsFixture *f, CsMetrics *m) {
    const XrXirModule *module = xr_xir_compile_artifact_module(f->lowered);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && d->module_count == 1 && !d->slot_count && module->function_count < CS_MAX_FUNCTIONS);
    f->module = d->root_module;
    f->main_entry = d->entry_function;
    CHECK(f->main_entry < module->function_count && !module->functions[f->main_entry].parameter_count);
    CHECK(module->functions[f->main_entry].result == XR_XIR_I64);
    f->owned_entry = cs_export(module, "owned", XR_XIR_STRING);
    CHECK(f->main_entry != f->owned_entry);
    size_t begin = instance_compile_attempts;
    for (uint32_t i = 0; i < module->function_count; ++i)
        CHECK(xr_xir_compile_vm_bind(f->lowered, i, &f->bindings[i], &f->entries[i]) == XR_XIR_OK);
    cs_compiler_step(m, 8, begin);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    const XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, f->entries, module->function_count,
        d, {f, cs_code_release}, module->types, xr_xir_compile_program_proof(f->lowered)};
    begin = instance_compile_attempts;
    CHECK(xr_xir_compile_program_seal(&f->compiler, &spec, &f->program) == XR_XIR_OK);
    cs_compiler_step(m, 9, begin);
    CHECK(f->program && !f->code_releases);
    CHECK(xr_compile_resources_stats(f->compiler.resources, &m->compiler) == XR_COMPILE_RESOURCE_OK);
}

static void cs_build(CsFixture *f, CsMetrics *m) {
    instance_compile_zero();
    CHECK(runtime_fail_at == SIZE_MAX && instance_compile_fail_at == SIZE_MAX);
    *f = (CsFixture){0};
    *m = (CsMetrics){0};
    size_t begin = instance_compile_attempts;
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&limits, &f->compiler.resources) == XR_COMPILE_RESOURCE_OK);
    f->compiler.limits = xr_xir_compile_default_limits();
    cs_fixture_source(f);
    cs_compiler_step(m, 0, begin);
    XrXirArtifact *checked = NULL;
    cs_source_checked(f, m, &checked);
    cs_packet_roundtrip(f, m, &checked);
    cs_lower(f, m, checked);
    cs_seal(f, m);
}
#endif // XIR_OWNED_STRING_SOURCE_RESOURCES_H
