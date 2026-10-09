/* Real Source and owned Checked round trips feed one target-bound Lowered image. */
#ifndef ORDINARY_LIVE_LIMIT_COMPILE_H
#define ORDINARY_LIVE_LIMIT_COMPILE_H
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_program.h"
#ifndef OLL_MODULES
#define OLL_MODULES 25u
#define OLL_FUNCTIONS 52u
#endif
#ifdef OLL_SOURCE
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#endif

typedef struct OllFixture { uint32_t entry, run, recovery, root; } OllFixture;
static const XrCompileResourceLimits oll_compile_caps = {
    UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};

static void oll_context(XrXirCompileContext *context) {
    *context = (XrXirCompileContext){0};
    CHECK(xr_compile_resources_new(&oll_compile_caps, &context->resources) == XR_COMPILE_RESOURCE_OK);
    context->limits = xr_xir_compile_default_limits();
}
#ifdef OLL_SOURCE
static uint32_t oll_export(const XrXirModule *module, const char *name) {
    const XrXirDeclarations *d = module->declarations;
    size_t length = strlen(name); uint32_t found = UINT32_MAX;
    CHECK(d && d->functions);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *id = &d->functions[f];
        if (fn->name_length != length || memcmp(fn->name, name, length) ||
            id->module != d->root_module || id->nominal_owner) continue;
        CHECK(found == UINT32_MAX && id->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
        found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}
static OllFixture oll_fixture(const XrXirArtifact *artifact) {
    const XrXirModule *m = xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->declarations && m->declarations->module_count == OLL_MODULES &&
        m->function_count == OLL_FUNCTIONS && !m->declarations->slot_count);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->root_module < OLL_MODULES && d->entry_function < m->function_count);
    for (uint32_t n = 0; n < d->module_count; ++n) {
        uint32_t initializer = d->modules[n].initializer;
        CHECK(initializer < m->function_count && !m->functions[initializer].parameter_count &&
            m->functions[initializer].result == XR_XIR_UNIT);
    }
    return (OllFixture){d->entry_function, oll_export(m, "run"), oll_export(m, "recovery"), d->root_module};
}
/* Each input artifact and serialized borrow dies before the next stage runs. */
static XrXirArtifact *oll_roundtrip(const XrXirCompileContext *context, XrXirArtifact *input) {
    XrXirCheckedPacket packet = {0}; XrXirArtifact *read = NULL;
    CHECK(xr_xir_compile_checked_write(input, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(input);
    CHECK(packet.bytes && packet.length);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(!packet.bytes && !packet.length && read);
    CHECK(xr_xir_compile_artifact_verify(read, NULL) == XR_XIR_OK);
    return read;
}
static XrXirArtifact *oll_lower(const XrXirCompileContext *context, XrXirArtifact *checked,
    const XrXirTarget *target, OllFixture *fixture) {
    checked = oll_roundtrip(context, checked);
    checked = oll_roundtrip(context, checked);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    *fixture = oll_fixture(closed);
    CHECK(xr_xir_compile_lower(closed, target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
#ifdef OLL_READY_GRAPH
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.layouts); uint32_t answers = 0, thick = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length != 6 || memcmp(fn->name, "answer", 6)) continue;
        ++answers; CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64 && fn->result == XR_XIR_I64);
        bool calls = false;
        for (uint32_t op = 0; op < fn->instruction_count; ++op) if (fn->instructions[op].op == XR_XIR_CALL) calls = true;
        CHECK(!proof.layouts[f].owned_count);
        if (calls) { CHECK(proof.layouts[f].frame_bytes >= 128 && proof.layouts[f].outgoing_count == 1); ++thick; }
    }
    CHECK(answers == 16 && thick == 15);
#endif
    OllFixture after = oll_fixture(lowered);
    CHECK(after.entry == fixture->entry && after.run == fixture->run &&
        after.recovery == fixture->recovery && after.root == fixture->root);
    return lowered;
}
static XrXirArtifact *oll_source(const XrXirCompileContext *context) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char entry[2048]; int length = snprintf(entry, sizeof(entry), "%s/root.xr", OLL_FIXTURE_DIRECTORY);
    CHECK(length > 0 && (size_t)length < sizeof(entry));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, OLL_FIXTURE_DIRECTORY};
    XrXirSourceRequest request = {session, entry, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    if (status != XR_XIR_OK) fprintf(stderr, "ordinary Source status=%u path=%s\n", (unsigned)status, failure ? failure : "");
    CHECK(status == XR_XIR_OK && diagnostic.status == status && result.checked && result.snapshot && !failure);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_resources_free(failure);
    xr_compile_session_free(session);
    memset(entry, 0xa5, sizeof(entry)); memset(&authority, 0xa5, sizeof(authority));
    memset(&request, 0xa5, sizeof(request));
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    return checked;
}
#endif
#endif
