/* One owned Source/Checked/specialization/Lowered route for every scalar family. */
#ifndef XIR_TASK_SCALAR_PIPELINE_H
#define XIR_TASK_SCALAR_PIPELINE_H
#include "xir/xxir_types.h"
#include "xir_task_scalar_oracles.h"
static XrXirArtifact *task_scalar_lower(const XrXirCompileContext *context,
    uint32_t entries[TASK_SCALAR_CASES][2], bool baseline) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_SCALAR_FIXTURES};
    XrXirSourceRequest request = {session, XR_TASK_SCALAR_FIXTURES "/root.xr", &authority,
        context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL; XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "Task scalar check=%u %s\n", status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && source.checked && !failure);
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (baseline) {
        CHECK(status == XR_XIR_UNSUPPORTED && !lowered);
        printf("TASK_SCALAR_BASELINE Source/Checked73/specialization=OK Lowered=UNSUPPORTED function=%u instruction=%u\n",
            diagnostic.function, diagnostic.instruction); return NULL;
    }
    if (status != XR_XIR_OK) fprintf(stderr, "Task scalar lower=%u function=%u instruction=%u\n", status,
        diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && lowered);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered); unsigned matches[TASK_SCALAR_CASES][2] = {{0}};
#ifdef XR_TASK_PARAMETER_FIXTURE
    unsigned workers=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length<7 || memcmp(function->name,"worker_",7)) continue;
        CHECK(function->parameter_count==1);++workers;
    }
    CHECK(workers==TASK_SCALAR_CASES);
#endif
    for (unsigned n = 0; n < TASK_SCALAR_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode) {
        char name[64]; CHECK(snprintf(name, sizeof(name), "%s_%s", mode ? "handle" : "await", task_scalar_oracles[n].name) > 0);
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
            if (identity->exported && identity->module == module->declarations->root_module && !identity->nominal_owner &&
                function->name_length == strlen(name) && !memcmp(function->name, name, function->name_length)) {
                CHECK(!function->parameter_count); entries[n][mode] = f; ++matches[n][mode];
                if (mode) {
                    const XrXirTypeNode *task = xr_xir_type_node(module->types, function->result);
                    CHECK(task && task->kind == XR_XIR_TYPE_TASK && task->element == task_scalar_oracles[n].type);
                } else CHECK(function->result == task_scalar_oracles[n].type);
            }
        }
        CHECK(matches[n][mode] == 1);
    }
    return lowered;
}
#endif // XIR_TASK_SCALAR_PIPELINE_H
