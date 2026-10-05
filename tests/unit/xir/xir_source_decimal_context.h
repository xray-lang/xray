/* Source precision must survive parsing before the Checked constant is sealed. */
#ifndef XIR_SOURCE_DECIMAL_CONTEXT_H
#define XIR_SOURCE_DECIMAL_CONTEXT_H
static void source_decimal_contexts(const XrXirSourceRequest *request, const char *root) {
    AdmissionCase case_owner={0};
    static const struct { const char *text; unsigned width; uint64_t expected; } cases[] = {
#define XIR_DECIMAL(text, width, expected) {text, width, expected},
#include "xir_decimal_vectors.def"
#undef XIR_DECIMAL
    };
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        /* Leading and trailing dot grammar is a separate lexer boundary. */
        if (cases[i].text[0] == '+' || !strcmp(cases[i].text, "1.")) continue;
        char source[4096];
        int length = snprintf(source, sizeof(source), "const value:f%u=(%s)\n", cases[i].width, cases[i].text);
        CHECK(length > 0 && (size_t) length < sizeof(source)); write_source(root, source);
        XrXirArtifact *artifact = NULL;
        XrXirSourceDiagnostic diagnostic;
        XrXirSourceResult query_result_1 = {0};
        admission_case_begin(request,&case_owner);
        XrXirStatus query_status_1 = xr_xir_compile_source_check(&case_owner.request, &query_result_1, &diagnostic,NULL);
        artifact = query_result_1.checked; query_result_1.checked = NULL;
        CHECK(query_status_1 == XR_XIR_OK || (!query_result_1.checked && !query_result_1.snapshot));
        xr_xir_compile_source_result_free(&query_result_1);
        XrXirStatus status = query_status_1;
        if (status != XR_XIR_OK) {
            XrCompileResourceStats stats=library_compile_stats(request->context);
            fprintf(stderr,"source decimal witness %zu width %u status%u allocated%llu live%llu peak%llu work%llu: %s\n",i,cases[i].width,status,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,diagnostic.message);
        }
        CHECK(status == XR_XIR_OK && artifact);
        const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
        const XrXirInstruction *op = &module->functions[0].instructions[0];
        CHECK(op->op == XR_XIR_CONST_FLOAT && op->type == (cases[i].width == 32 ? XR_XIR_F32 : XR_XIR_F64));
        uint64_t bits; memcpy(&bits, &op->immediate, sizeof(bits));
        CHECK(bits == cases[i].expected);
        xr_xir_compile_artifact_free(artifact); ++count;
    }
    printf("Source decimal precision: %u exact Checked constants\n", count);
    admission_case_end(&case_owner);
}
#endif // XIR_SOURCE_DECIMAL_CONTEXT_H
