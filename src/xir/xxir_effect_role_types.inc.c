/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_types.inc.c - Final-owned local role closure claims
 *
 * Every positive claim names an actual use and a decreasing path to a seed.
 * Neither the claim nor its storage shape confers admission by itself.
 */
typedef struct EffectRoleFact {
    uint32_t level,distance,instruction,ordinal;
} EffectRoleFact;
typedef struct EffectRoleFunction {
    uint32_t parameters,instructions,first,count,formal_first;
} EffectRoleFunction;
typedef struct EffectRoleProof {
    XrCompileResources *resources;
    uint32_t functions,total;
    EffectRoleFunction *rows;
    EffectRoleFact *facts;
} EffectRoleProof;

static void effect_role_proof_clear(EffectRoleProof *proof);
static XrXirStatus effect_role_proof_build(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,EffectRoleProof *proof);
static XrXirStatus effect_role_proof_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof);

static XrXirStatus effect_role_parameter_value(const EffectBodyRead *read,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    uint32_t function,uint32_t parameter,uint32_t *output);
static XrXirStatus effect_role_parameter_read(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    uint32_t function,uint32_t parameter,uint32_t *output);
static XrXirStatus effect_role_contracts_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    const XrXirFunctionEffectContract *contracts,uint32_t count);

/* A genuine callable formal selects one row for a callback-free read operation. It borrows actual
 * Input and Final storage; it stores no role values or admission decision. */
typedef struct EffectRoleParametersRead {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *material;
    const EffectRoleProof *proof;
    uint32_t function;
} EffectRoleParametersRead;

static XrXirStatus effect_role_parameters_open(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,uint32_t function,
    uint32_t callable_parameter,EffectRoleParametersRead *output);
static XrXirStatus effect_role_parameters_value(const EffectRoleParametersRead *source,
    uint32_t parameter,uint32_t *output);
