/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * original_array_cleanup_native.inc.c - Generated code and owned mixed bindings
 *
 * KEY CONCEPT:
 *   Fresh owned C bytes and complete proof/layouts bind static callbacks to the
 *   same Lowered artifact. Failed sealing publishes no Program or code lease.
 */
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"

typedef struct ArrayCleanupNativeImage {
    const XrXirProgramSpec *program;
    const char *prefix, *compiled_sha;
} ArrayCleanupNativeImage;
typedef struct ArrayCleanupMixedOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} ArrayCleanupMixedOwner;

static inline void array_cleanup_mixed_free(void *pointer) {
    ArrayCleanupMixedOwner *owner = pointer;
    if (!owner) return;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner);
}
static inline XrXirStatus array_cleanup_allocate(const XrXirCompileContext *context,
    size_t count, size_t size, void **output) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, size, output);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}
/* Both emitter owners are released on mismatch or failure. The caller sees a
 * complete first translation unit only after both real emissions succeed. */
static inline XrXirStatus array_cleanup_emit_lowered(const XrXirArtifact *lowered,
    const char *prefix, XrXirCSource *output, char digest[65]) {
    if (!lowered || !prefix || !output || output->text || output->length || !digest)
        return XR_XIR_BAD_STRUCTURE;
    XrXirCSource first = {0}, second = {0};
    XrXirStatus status = xr_xir_compile_emit_c(lowered, prefix, 16777216u, &first);
    if (status == XR_XIR_OK) status = xr_xir_compile_emit_c(lowered, prefix, 16777216u, &second);
    if (status == XR_XIR_OK && (!first.text || !second.text || !first.length ||
        first.text == second.text || first.length != second.length ||
        memcmp(first.text, second.text, first.length))) status = XR_XIR_BAD_VALUE;
    if (status == XR_XIR_OK) {
        static const char digits[] = "0123456789abcdef";
        uint8_t bytes[32]; char actual[65]; xr_sha256((const uint8_t *)first.text, first.length, bytes);
        for (unsigned i = 0; i < 32; ++i) { actual[i * 2] = digits[bytes[i] >> 4]; actual[i * 2 + 1] = digits[bytes[i] & 15u]; }
        actual[64] = 0; memcpy(digest, actual, sizeof(actual)); *output = first; first = (XrXirCSource){0};
    }
    xr_xir_compile_c_source_free(&second); xr_xir_compile_c_source_free(&first); return status;
}
/* The build command supplies a private temporary path and promotes it to the
 * registered generated-C output only after this complete write succeeds. */
static inline XrXirStatus array_cleanup_write_native(const XrXirArtifact *lowered, const char *output) {
    if (!output || !output[0]) return XR_XIR_BAD_STRUCTURE;
    XrXirCSource source = {0}; char digest[65] = {0};
    XrXirStatus status = array_cleanup_emit_lowered(lowered, "array_cleanup_native", &source, digest);
    if (status != XR_XIR_OK) return status;
    FILE *file = fopen(output, "wb");
    if (!file) { xr_xir_compile_c_source_free(&source); return XR_XIR_IO; }
    bool written = fwrite(source.text, 1, source.length, file) == source.length;
    if (written) written = fprintf(file, "\nconst char array_cleanup_native_c_sha[65]=\"%s\";\n", digest) > 0;
    int closed = fclose(file); xr_xir_compile_c_source_free(&source);
    return written && closed == 0 ? XR_XIR_OK : XR_XIR_IO;
}
static inline bool array_cleanup_layout_same(const XrXirFunctionLayout *a,
    const XrXirFunctionLayout *b, uint32_t parameters) {
    if (a->slot_count != b->slot_count || a->frame_bytes != b->frame_bytes || a->owned_count != b->owned_count ||
        a->outgoing_count != b->outgoing_count || a->path_count != b->path_count ||
        a->result.size != b->result.size || a->result.alignment != b->result.alignment) return false;
    if (a->slot_count && (!a->offsets || !b->offsets || memcmp(a->offsets, b->offsets, (size_t)a->slot_count * sizeof(uint32_t)))) return false;
    if (a->owned_count && (!a->owned_offsets || !b->owned_offsets || memcmp(a->owned_offsets, b->owned_offsets, (size_t)a->owned_count * sizeof(uint32_t)))) return false;
    if (parameters && (!a->parameters || !b->parameters || memcmp(a->parameters, b->parameters, (size_t)parameters * sizeof(XrXirLayout)))) return false;
    return true;
}
static inline bool array_cleanup_native_correspondence(const XrXirArtifact *lowered,
    const XrXirProgramSpec *spec) {
    const XrXirModule *m = xr_xir_compile_artifact_module(lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    if (!m || m->stage != XR_XIR_LOWERED || !m->declarations || !spec ||
        spec->abi_version != XR_XIR_PROGRAM_ABI_VERSION || spec->target.architecture != XR_XIR_ARCH_X86_64 ||
        spec->target.abi_version != XR_XIR_VALUE_ABI_VERSION || spec->entry_count != m->function_count ||
        !spec->entries || !spec->declarations || spec->code.owner || spec->code.release ||
        !proof.bytes || !proof.identity || !proof.layouts || !spec->proof.bytes || !spec->proof.identity || !spec->proof.layouts ||
        proof.length != spec->proof.length || memcmp(proof.bytes, spec->proof.bytes, proof.length) ||
        memcmp(proof.identity, spec->proof.identity, 32)) return false;
    if (spec->declarations->root_module != m->declarations->root_module ||
        spec->declarations->entry_function != m->declarations->entry_function ||
        spec->declarations->module_count != m->declarations->module_count ||
        !spec->declarations->functions) return false;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f]; const XrXirCallEntry *entry = &spec->entries[f];
        const XrXirFunctionIdentity *id = &m->declarations->functions[f];
        if (memcmp(&spec->declarations->functions[f], id, sizeof(*id)) ||
            !array_cleanup_layout_same(&proof.layouts[f], &spec->proof.layouts[f], fn->parameter_count) ||
            entry->abi_version != XR_XIR_CALL_ABI_VERSION || !entry->resume || !entry->release ||
            entry->parameter_count != fn->parameter_count || entry->result != fn->result ||
            entry->cleanup_owner != id->cleanup_owner || (entry->flags & ~XR_XIR_ENTRY_EXIT)) return false;
        if (fn->parameter_count && (!entry->parameters || !fn->parameters ||
            memcmp(entry->parameters, fn->parameters, (size_t)fn->parameter_count * sizeof(XrXirType)))) return false;
    }
    return true;
}
/* mode1 copies real generated callbacks. mode2 uses native even function IDs;
 * mode3 uses native odd IDs. Every opposite entry is a real public VM binding.
 * On mixed success the Program lease consumes and clears the input artifact.
 * Failure preserves the input artifact and the caller's empty Program output. */
static inline XrXirStatus array_cleanup_native_seal(const XrXirCompileContext *context,
    XrXirArtifact **lowered, const ArrayCleanupNativeImage *image, unsigned mode, XrXirProgram **output) {
    if (!context || !context->resources || !lowered || !*lowered || !image || !image->program ||
        !image->prefix || !image->compiled_sha || mode < 1 || mode > 3 || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirCSource generated = {0}; char digest[65] = {0};
    XrXirStatus status = array_cleanup_emit_lowered(*lowered, image->prefix, &generated, digest);
    xr_xir_compile_c_source_free(&generated);
    if (status != XR_XIR_OK) return status;
    if (strcmp(digest, image->compiled_sha) || !array_cleanup_native_correspondence(*lowered, image->program))
        return XR_XIR_BAD_VALUE;
    if (mode == 1) return xr_xir_compile_program_seal(context, image->program, output);
    ArrayCleanupMixedOwner *owner = NULL;
    status = array_cleanup_allocate(context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK) return status;
    const XrXirModule *m = xr_xir_compile_artifact_module(*lowered);
    status = array_cleanup_allocate(context, m->function_count, sizeof(*owner->entries), (void **)&owner->entries);
    if (status == XR_XIR_OK) status = array_cleanup_allocate(context, m->function_count, sizeof(*owner->bindings), (void **)&owner->bindings);
    if (status != XR_XIR_OK) goto failed;
    unsigned native = 0, vm = 0;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        if ((f % 2u == 0) == (mode == 2)) { owner->entries[f] = image->program->entries[f]; ++native; }
        else { status = xr_xir_compile_vm_bind(*lowered, f, &owner->bindings[f], &owner->entries[f]); ++vm; }
        if (status != XR_XIR_OK) goto failed;
        if (owner->entries[f].flags != image->program->entries[f].flags ||
            owner->entries[f].cleanup_owner != image->program->entries[f].cleanup_owner) { status = XR_XIR_BAD_VALUE; goto failed; }
    }
    if (!native || !vm) { status = XR_XIR_BAD_VALUE; goto failed; }
    owner->lowered = *lowered;
    XrXirProgramSpec spec = *image->program; spec.entries = owner->entries;
    spec.declarations = m->declarations; spec.types = m->types;
    spec.proof = xr_xir_compile_program_proof(*lowered); spec.code = (XrXirCodeLease){owner, array_cleanup_mixed_free};
    status = xr_xir_compile_program_seal(context, &spec, output);
    if (status == XR_XIR_OK) { *lowered = NULL; return status; }
    /* Sealing takes the lease only on success; preserve the caller's artifact. */
    owner->lowered = NULL;
failed:
    array_cleanup_mixed_free(owner); return status;
}

/* Each native executable links exactly one family's actual generated C unit.
 * The same external names are safe across distinct executable targets. */
#ifdef ARRAY_CLEANUP_NATIVE
extern const XrXirProgramSpec array_cleanup_native_program;
extern const char array_cleanup_native_c_sha[65];
static inline const ArrayCleanupNativeImage *array_cleanup_compiled_image(void) {
    static const ArrayCleanupNativeImage image = {
        &array_cleanup_native_program, "array_cleanup_native", array_cleanup_native_c_sha
    };
    return &image;
}
#else
static inline const ArrayCleanupNativeImage *array_cleanup_compiled_image(void) { return NULL; }
#endif
