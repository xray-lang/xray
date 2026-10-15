/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_specialize_key_order_cases.h - Independent long-key discovery witnesses
 *
 * KEY CONCEPT:
 *   Literal call order and type vectors define the expected identities without
 *   consulting a production hash, lookup table, or emitted instance name.
 */
#ifndef XIR_SPECIALIZE_KEY_ORDER_CASES_H
#define XIR_SPECIALIZE_KEY_ORDER_CASES_H

enum { SPEC_KEY_WIDTH = 16, SPEC_KEY_CALLS = 10, SPEC_KEY_UNIQUE = 5 };
static const uint32_t spec_key_calls[SPEC_KEY_CALLS] = {2,4,1,2,3,0,4,1,0,3};
static const uint32_t spec_key_ranks[SPEC_KEY_CALLS] = {0,1,2,0,3,4,1,2,4,3};

static const char *spec_key_type_name(uint32_t key, uint32_t argument) {
    CHECK(key < SPEC_KEY_UNIQUE && argument < SPEC_KEY_WIDTH);
    if (argument == 0) {
        if (key == 1 || key == 4) return "i32";
        if (key == 3) return "i16";
    }
    if (argument == SPEC_KEY_WIDTH - 1) {
        if (key == 2 || key == 3) return "i32";
        if (key == 4) return "i16";
    }
    return "i64";
}

static void spec_key_source_text(char *text, size_t capacity) {
    size_t length = 0; text[0] = 0;
    for (uint32_t declaration = 0; declaration < 2; ++declaration) {
        spec_index_text_append(text,capacity,&length,declaration ? "fn twin<" : "fn stamp<");
        for (uint32_t argument = 0; argument < SPEC_KEY_WIDTH; ++argument) {
            char parameter[16];
            CHECK(snprintf(parameter,sizeof(parameter),"%sT%u",argument ? "," : "",argument) > 0);
            spec_index_text_append(text,capacity,&length,parameter);
        }
        spec_index_text_append(text,capacity,&length,">(v:i64)->i64{return v}\n");
    }
    spec_index_text_append(text,capacity,&length,"export fn run()->i64{var sum=0;");
    for (uint32_t call = 0; call < SPEC_KEY_CALLS + 2; ++call) {
        uint32_t key = call < SPEC_KEY_CALLS ? spec_key_calls[call] : 2;
        spec_index_text_append(text,capacity,&length,call == SPEC_KEY_CALLS ? "sum=sum+twin<" : "sum=sum+stamp<");
        for (uint32_t argument = 0; argument < SPEC_KEY_WIDTH; ++argument) {
            if (argument) spec_index_text_append(text,capacity,&length,",");
            spec_index_text_append(text,capacity,&length,spec_key_type_name(key,argument));
        }
        spec_index_text_append(text,capacity,&length,">(7);");
    }
    spec_index_text_append(text,capacity,&length,"return sum}\nprint(run())\n");
}

static void spec_key_origins(const XrXirModule *source, const XrXirModule *instance) {
    uint32_t stamp = spec_index_source_function(source,"stamp");
    uint32_t twin = spec_index_source_function(source,"twin");
    uint32_t run = spec_index_source_function(source,"run");
    const XrXirProvenance *proof = instance->provenance;
    CHECK(proof && proof->kind == XR_XIR_EVIDENCE_INSTANCE);
    uint32_t selected = UINT32_MAX, stamp_count = 0, twin_count = 0;
    for (uint32_t f = 0; f < proof->count; ++f) {
        if (proof->origins[f].function == run) { CHECK(selected == UINT32_MAX); selected = f; }
        stamp_count += proof->origins[f].function == stamp;
        twin_count += proof->origins[f].function == twin;
    }
    CHECK(selected != UINT32_MAX && stamp_count == SPEC_KEY_UNIQUE && twin_count == 1);
    const XrXirFunction *before = &source->functions[run], *after = &instance->functions[selected];
    CHECK(before->instruction_count == after->instruction_count);
    uint32_t ids[SPEC_KEY_UNIQUE] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    uint32_t calls = 0, unique = 0, twin_id = UINT32_MAX;
    /* Endpoints are literal semantic expectations, separate from text generation. */
    const XrXirType first[SPEC_KEY_UNIQUE] = {XR_XIR_I64,XR_XIR_I32,XR_XIR_I32,XR_XIR_I16,XR_XIR_I64};
    const XrXirType last[SPEC_KEY_UNIQUE] = {XR_XIR_I32,XR_XIR_I16,XR_XIR_I64,XR_XIR_I32,XR_XIR_I64};
    for (uint32_t i = 0; i < before->instruction_count; ++i) {
        const XrXirInstruction *op = &before->instructions[i], *actual = &after->instructions[i];
        if (op->op != XR_XIR_CALL || (op->immediate != stamp && op->immediate != twin)) continue;
        CHECK(calls < SPEC_KEY_CALLS + 2 && actual->op == XR_XIR_CALL && actual->immediate >= 0 &&
            (uint64_t)actual->immediate < proof->count);
        uint32_t id = (uint32_t)actual->immediate;
        uint32_t rank = calls < SPEC_KEY_CALLS ? spec_key_ranks[calls] : 0;
        const XrXirOrigin *origin = &proof->origins[id];
        CHECK(origin->function == (calls == SPEC_KEY_CALLS ? twin : stamp));
        CHECK(origin->argument_count == SPEC_KEY_WIDTH && origin->effect_argument_count == 1);
        CHECK(origin->arguments[0] == first[rank] && origin->arguments[SPEC_KEY_WIDTH - 1] == last[rank]);
        for (uint32_t a = 1; a + 1 < SPEC_KEY_WIDTH; ++a) CHECK(origin->arguments[a] == XR_XIR_I64);
        CHECK(origin->effect_arguments[0].parameter == 0 && origin->effect_arguments[0].type == XR_XIR_I64);
        CHECK(!instance->declarations->functions[id].cleanup_owner);
        if (calls == SPEC_KEY_CALLS) {
            twin_id = id;
            for (uint32_t k = 0; k < SPEC_KEY_UNIQUE; ++k) CHECK(twin_id != ids[k]);
        } else if (ids[rank] == UINT32_MAX) {
            CHECK(rank == unique);
            if (unique) CHECK(ids[unique - 1] < id);
            ids[rank] = id; ++unique;
        } else CHECK(ids[rank] == id);
        ++calls;
    }
    CHECK(calls == SPEC_KEY_CALLS + 2 && unique == SPEC_KEY_UNIQUE && twin_id != UINT32_MAX);
}

static void spec_key_source_controls(void) {
    LibraryCompileOwner owner = {0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits) == XR_XIR_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY,"specialize-key-order",NULL};
    XrXirSourceRequest request = {session,NULL,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL,NULL};
    char text[8192]; spec_key_source_text(text,sizeof(text));
    XrXirSourceText input = {NULL,text,strlen(text)};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check_text(&request,&input,&source,&source_diagnostic,&failure);
    if (status != XR_XIR_OK) fprintf(stderr,"long-key Source status%u at %d:%d: %s\n",
        (unsigned)status,source_diagnostic.line,source_diagnostic.column,source_diagnostic.message);
    CHECK(status == XR_XIR_OK);
    xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *read = NULL, *instance = NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,&diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&read,&diagnostic) == XR_XIR_OK);
    memset(packet.bytes,0,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&instance,&diagnostic) == XR_XIR_OK);
    spec_key_origins(&read->module,&instance->module);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(instance,&diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(instance); xr_compile_resources_free(failure);
    CHECK(library_compile_stats(&owner.context).live_bytes == owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);
    puts("specialization 16-element keys, endpoint/order distinctions, declaration separation, first-discovery reuse and producer death PASS");
}
#endif // XIR_SPECIALIZE_KEY_ORDER_CASES_H
