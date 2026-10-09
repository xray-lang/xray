/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_typed_native_bounds.h - Fixed callable bounds across owned import
 *
 * KEY CONCEPT:
 *   An explicitly typed function field retains its unknown upper bound even
 *   when this particular program stores a pure function in that field.
 */
#ifndef XIR_LIBRARY_TYPED_NATIVE_BOUNDS_H
#define XIR_LIBRARY_TYPED_NATIVE_BOUNDS_H
#include "xir/xxir_types.h"

static bool typed_bound_name(const char *bytes,uint32_t length,const char *expected) {
    return length==strlen(expected) && !memcmp(bytes,expected,length);
}
static void typed_carrier_bounds(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module && module->types && module->types->nominals && module->declarations);
    const XrXirNominalTable *table=module->types->nominals;
    uint32_t dispatch=UINT32_MAX;
    for(uint32_t n=0;n<table->count;++n) {
        const XrXirNominalDeclaration *decl=&table->declarations[n];
        if(!typed_bound_name(decl->name.bytes,decl->name.length,"Dispatch"))continue;
        CHECK(dispatch==UINT32_MAX);dispatch=n;
    }
    CHECK(dispatch!=UINT32_MAX);
    const XrXirNominalDeclaration *decl=&table->declarations[dispatch];
    CHECK(decl->field_count==1 &&
        typed_bound_name(decl->fields[0].name.bytes,decl->fields[0].name.length,"invoke"));
    const XrXirTypeNode *field=xr_xir_callable_signature(module->types,decl->fields[0].type);
    CHECK(field && field->parameter_count==1 && field->parameters[0].type==XR_XIR_I64 &&
        field->result==XR_XIR_I64 && field->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    const XrXirProvenance *evidence=module->provenance;
    CHECK(evidence && evidence->kind==XR_XIR_EVIDENCE_TEMPLATE &&
        evidence->contract_count==module->function_count && evidence->contracts);
    bool run=false,increment=false;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        if(identity->nominal_owner==dispatch+1 &&
            typed_bound_name(function->name,function->name_length,"run")) {
            CHECK(!run);run=true;
            CHECK(evidence->contracts[f].formula.constant_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        }
        if(!identity->nominal_owner &&
            typed_bound_name(function->name,function->name_length,"increment")) {
            CHECK(!increment);increment=true;
            const XrXirTypeNode *result=xr_xir_callable_signature(module->types,function->result);
            CHECK(result && result->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        }
    }
    CHECK(run && increment);
}

#endif // XIR_LIBRARY_TYPED_NATIVE_BOUNDS_H
