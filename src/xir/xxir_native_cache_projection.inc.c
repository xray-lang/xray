/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_cache_projection.inc.c - Ordinal-independent primitive leaf keys
 *
 * KEY CONCEPT: Only locally indexed, dependency-free leaves have portable keys.
 */
#include "xxir_compile_memory.h"
#include "xxir_checked.h"
#include "xxir_effects.h"
#include "xxir_atomic.h"
#include "xxir_nominal.h"
#include "xxir_output.h"
#include "../execution/xr_xir_host_execution.h"
#include "../base/xsha256.h"
#include <limits.h>
#include <float.h>

static bool cache_hash_bytes(const XrXirCompileContext *context, XrSHA256Context *hash,
    const void *bytes, size_t length) {
    if (!xir_compile_work(context, length)) return false;
    xr_sha256_update(hash, bytes, length); return true;
}
static bool cache_hash_word(const XrXirCompileContext *context, XrSHA256Context *hash, uint64_t word) {
    uint8_t bytes[8];
    if (!xir_compile_work(context, 8)) return false;
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(word >> (i * 8));
    return cache_hash_bytes(context, hash, bytes, sizeof(bytes));
}
static bool cache_hash_frame(const XrXirCompileContext *context, XrSHA256Context *hash,
    const void *bytes, size_t length) {
    return cache_hash_word(context, hash, length) && cache_hash_bytes(context, hash, bytes, length);
}
static XrXirStatus cache_semantic_id(const XrXirCompileContext *context,
    const void *packet, size_t length, uint8_t output[32]) {
    static const char domain[] = "xir-checked-native-pair-v1";
    static const char identity[] = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    XrSHA256Context hash; xr_sha256_init(&hash);
    if (!cache_hash_frame(context,&hash,domain,sizeof(domain)-1) ||
        !cache_hash_word(context,&hash,XR_XIR_CHECKED_CONTRACT) ||
        !cache_hash_word(context,&hash,1) ||
        !cache_hash_frame(context,&hash,identity,sizeof(identity)-1) ||
        !cache_hash_frame(context,&hash,packet,length) || !cache_hash_word(context,&hash,0))
        return XR_XIR_BUDGET;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,output); return XR_XIR_OK;
}
static XrXirStatus cache_abi_id(const XrXirCompileContext *context, uint8_t output[32]) {
    XrSHA256Context hash; const uint32_t endian = 1;
    xr_sha256_init(&hash);
    if (!cache_hash_word(context,&hash,*(const uint8_t *)&endian)) return XR_XIR_BUDGET;
#define XR_XIR_SDK_ABI(id, expression) \
    if (!cache_hash_word(context,&hash,(id)) || !cache_hash_word(context,&hash,(uint64_t)(expression))) return XR_XIR_BUDGET;
#include "../toolchain/xr_xir_runtime_sdk_abi.def"
#undef XR_XIR_SDK_ABI
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,output); return XR_XIR_OK;
}
/* The caller owns a fully verified immutable Lowered artifact for this query. */
static XrXirStatus cache_leaf_id(const XrXirArtifact *artifact, uint32_t index,
    uint32_t stream, uint8_t output[32]) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact,index);
    const char *name = stream == 1 ? "writeStdout" : "writeStderr";
    static const char identity[] = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    if (!module || module->stage != XR_XIR_LOWERED || !module->declarations ||
        index >= module->function_count || !layout) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    const XrXirFunction *function = &module->functions[index];
    const XrXirFunctionIdentity *declaration = &module->declarations->functions[index];
    if (declaration->module >= module->declarations->module_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *owner = &module->declarations->modules[declaration->module];
    if (!xir_compile_work(context,sizeof(identity)-1+strlen(name))) return XR_XIR_BUDGET;
    if (owner->name_length != sizeof(identity)-1 || memcmp(owner->name,identity,sizeof(identity)-1) ||
        function->name_length != strlen(name) || memcmp(function->name,name,strlen(name))) return XR_XIR_UNRESOLVED;
    if (function->parameter_count != 1 || function->parameters[0] != XR_XIR_STRING ||
        function->result != XR_XIR_BOOL || declaration->exported != 1 || declaration->nominal_owner ||
        declaration->member_access || declaration->cleanup_owner || declaration->method_kind ||
        declaration->test_role || declaration->test_timeout_seconds || owner->dependency_count ||
        (module->generics && (module->generics[index].parameter_count || module->generics[index].argument_count)))
        return XR_XIR_BAD_TYPE;
    XrSHA256Context hash; xr_sha256_init(&hash);
#define CACHE_WORD(value) do { if (!cache_hash_word(context,&hash,(value))) return XR_XIR_BUDGET; } while (0)
    if (!cache_hash_frame(context,&hash,identity,sizeof(identity)-1) ||
        !cache_hash_frame(context,&hash,name,strlen(name))) return XR_XIR_BUDGET;
    CACHE_WORD(XR_XIR_CHECKED_CONTRACT); CACHE_WORD(declaration->exported);
    CACHE_WORD(declaration->promises); CACHE_WORD(declaration->member_access);
    CACHE_WORD(declaration->method_kind); CACHE_WORD(declaration->cleanup_owner);
    /* Exact empty type arguments, constraints, witnesses, callbacks and captures. */
    for (unsigned empty = 0; empty < 5; ++empty) CACHE_WORD(0);
    CACHE_WORD(function->parameter_count); CACHE_WORD(function->parameters[0]); CACHE_WORD(0);
    CACHE_WORD(function->result); CACHE_WORD(function->block_count); CACHE_WORD(function->instruction_count);
    CACHE_WORD(function->operand_count);
    unsigned writes = 0;
    for (uint32_t b = 0; b < function->block_count; ++b) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirBlock *block = &function->blocks[b];
        if (block->panic || block->frontier) return XR_XIR_UNSUPPORTED;
        CACHE_WORD(block->first); CACHE_WORD(block->count); CACHE_WORD(block->panic); CACHE_WORD(block->frontier);
    }
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *instruction = &function->instructions[i];
        if (instruction->op != XR_XIR_WRITE_STREAM && instruction->op != XR_XIR_RETURN &&
            instruction->op != XR_XIR_SCALAR_COPY) return XR_XIR_UNSUPPORTED;
        if (instruction->type != XR_XIR_UNIT && instruction->type != XR_XIR_STRING && instruction->type != XR_XIR_BOOL)
            return XR_XIR_UNSUPPORTED;
        if (instruction->type_arguments[0] || instruction->type_arguments[1]) return XR_XIR_UNSUPPORTED;
        if (instruction->op == XR_XIR_WRITE_STREAM) {
            if (instruction->immediate != stream) return XR_XIR_BAD_VALUE; ++writes;
        }
        CACHE_WORD(instruction->op); CACHE_WORD(instruction->type);
        CACHE_WORD(instruction->args[0]); CACHE_WORD(instruction->args[1]);
        CACHE_WORD(instruction->targets[0]); CACHE_WORD(instruction->targets[1]);
        CACHE_WORD((uint64_t)instruction->immediate); CACHE_WORD(0); CACHE_WORD(0);
    }
    if (writes != 1 || function->operand_count) return XR_XIR_UNSUPPORTED;
    /* Complete local canonical frame layout, without any old module ordinal. */
    CACHE_WORD(layout->slot_count); CACHE_WORD(layout->frame_bytes);
    for (uint32_t i = 0; i < layout->slot_count; ++i) CACHE_WORD(layout->offsets[i]);
    CACHE_WORD(layout->parameters[0].size); CACHE_WORD(layout->parameters[0].alignment);
    CACHE_WORD(layout->result.size); CACHE_WORD(layout->result.alignment); CACHE_WORD(layout->owned_count);
    for (uint32_t i = 0; i < layout->owned_count; ++i) CACHE_WORD(layout->owned_offsets[i]);
    CACHE_WORD(layout->outgoing_count); CACHE_WORD(layout->path_count);
    /* This whitelist cannot call, throw, suspend, capture or depend on another declaration. */
    CACHE_WORD(XR_XIR_EFFECT_NONE); CACHE_WORD(XR_XIR_EFFECT_NONE); CACHE_WORD(0);
#undef CACHE_WORD
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,output); return XR_XIR_OK;
}
