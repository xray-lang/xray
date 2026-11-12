/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_types.inc.c - Finite borrowed storage and declared return shapes
 *
 * KEY CONCEPT:
 *   Earlier descriptor edges propagate borrowed leaves without expanding a DAG.
 */
#include "xxir_view_access_internal.h"
#include "xxir_construction_internal.h"
#include "xxir_native_callable.h"
#include "xxir_receive_control.h"
static bool view_pointer(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && (node->kind==XR_XIR_TYPE_PTR || node->kind==XR_XIR_TYPE_MUT_PTR);
}
static bool view_pointer_result(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return view_pointer(types,type) || (node && node->kind==XR_XIR_TYPE_NULLABLE &&
        view_pointer(types,node->element));
}
static uint32_t view_flags_at(const XrXirTypes *types,const uint8_t *flags,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && flags ? flags[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]&3u : 0;
}
static uint32_t view_leaves_at(const XrXirTypes *types,const uint8_t *flags,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && flags ? flags[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]>>2 : 0;
}

/* The descriptor pass has already checked every edge and payload. A single
 * ascending walk visits each physical Tuple edge once, including shared DAGs. */
static XrXirStatus view_flags_build_mode(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t **output,bool pointers) {
    *output=NULL;
    if (!types) return XR_XIR_OK;
    if (!!types->count!=!!types->nodes) return XR_XIR_BAD_STRUCTURE;
    bool present=false;
    for (uint32_t i=0;i<types->count;++i) {

        present|=types->nodes[i].kind==XR_XIR_TYPE_SLICE || types->nodes[i].kind==XR_XIR_TYPE_MUT_SLICE ||
            (pointers && view_pointer(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i)));
    }
    if (!present) return XR_XIR_OK;
    if (types->count>context->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint8_t *flags=xir_compile_calloc(context,types->count,sizeof(*flags),&status);
    if (!flags) return status;
    for (uint32_t i=0;i<types->count && status==XR_XIR_OK;++i) {

        const XrXirTypeNode *node=&types->nodes[i];
        if (node->kind==XR_XIR_TYPE_SLICE) flags[i]=XR_XIR_VIEW_SHARED|4u;
        else if (node->kind==XR_XIR_TYPE_MUT_SLICE) flags[i]=XR_XIR_VIEW_MUTABLE|4u;
        else if (pointers && view_pointer(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i)))
            flags[i]=XR_XIR_VIEW_SHARED|4u;
        else if (node->kind==XR_XIR_TYPE_NULLABLE) flags[i]=(uint8_t)(view_flags_at(types,flags,node->element)|
            (view_leaves_at(types,flags,node->element)<<2));
        else if (node->kind==XR_XIR_TYPE_TUPLE)
            for (uint32_t p=0;p<node->parameter_count;++p) {

                flags[i]|=(uint8_t)view_flags_at(types,flags,node->parameters[p].type);
                uint32_t leaves=(flags[i]>>2)+view_leaves_at(types,flags,node->parameters[p].type);
                flags[i]=(uint8_t)((flags[i]&3u)|((leaves>1?2u:leaves)<<2));
            }
    }
    if (status==XR_XIR_OK) *output=flags;else xr_compile_resources_free(flags);
    return status;
}
static XrXirStatus view_flags_build(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t **output) {
    return view_flags_build_mode(context,types,output,false);
}
XR_FUNC XrXirStatus xir_compile_view_static_flags_new(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    return view_flags_build_mode(context,types,output,true);
}
XR_FUNC XrXirStatus xir_compile_view_flags_new(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    return view_flags_build(context,types,output);
}

static XrXirStatus view_return_candidate(
    const XrXirTypes *types,const uint8_t *flags,const XrXirTypeNode *node,uint32_t *output) {
    uint32_t result=view_flags_at(types,flags,node->result),candidate=UINT32_MAX;
    if (result) {
        uint32_t candidates=0,permission=0;
        for (uint32_t p=0;p<node->parameter_count;++p) {

            const XrXirCallableParameter *parameter=&node->parameters[p];
            uint32_t borrowed=view_flags_at(types,flags,parameter->type);
            if (parameter->mode!=XR_PARAM_REF && !borrowed) continue;
            uint32_t leaves=parameter->mode==XR_PARAM_REF ? 1u : view_leaves_at(types,flags,parameter->type);
            candidates=candidates>1 || leaves>1 ? 2u : candidates+leaves;candidate=p;
            permission=parameter->mode==XR_PARAM_REF ? XR_XIR_VIEW_MUTABLE : borrowed;
        }
        if (!candidates && view_pointer_result(types,node->result)) {*output=UINT32_MAX-1u;return XR_XIR_OK;}
        if (candidates!=1 || ((result&XR_XIR_VIEW_MUTABLE) && !(permission&XR_XIR_VIEW_MUTABLE)))
            return XR_XIR_BAD_TYPE;
    }
    *output=candidate;return XR_XIR_OK;
}

static XrXirStatus view_types_verify(const XrXirCompileContext *context,const XrXirTypes *types) {
    uint8_t *flags=NULL;
    XrXirStatus status=view_flags_build(context,types,&flags);
    if (status!=XR_XIR_OK || !flags) return status;
    for (uint32_t i=0;i<types->count && status==XR_XIR_OK;++i) {

        const XrXirTypeNode *node=&types->nodes[i];
        if (node->kind==XR_XIR_TYPE_CALLABLE) {
            for(uint32_t p=0;p<node->parameter_count;++p)
                if(node->parameters[p].mode==XR_PARAM_MOVE && view_flags_at(types,flags,node->parameters[p].type)) {
                    status=XR_XIR_BAD_TYPE;break;
                }
            if(status!=XR_XIR_OK)break;
            uint32_t candidate;
            status=view_return_candidate(types,flags,node,&candidate);
        } else if (node->kind==XR_XIR_TYPE_NOMINAL) {
            for (uint32_t a=0;a<node->nominal.argument_count && status==XR_XIR_OK;++a) {
                if (view_flags_at(types,flags,node->nominal.arguments[a])) status=XR_XIR_BAD_TYPE;
            }
            for (uint32_t f=0;f<node->nominal.field_count && status==XR_XIR_OK;++f) {
                if (view_flags_at(types,flags,node->nominal.fields[f])) status=XR_XIR_BAD_TYPE;
            }
        } else if (node->kind!=XR_XIR_TYPE_TUPLE && node->kind!=XR_XIR_TYPE_NULLABLE) {
            if (view_flags_at(types,flags,node->element) || view_flags_at(types,flags,node->result))
                status=XR_XIR_BAD_TYPE;
        }
    }
    xr_compile_resources_free(flags);return status;
}

XR_FUNC XrXirStatus xr_xir_compile_view_type_flags(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type,uint32_t *output) {
    if (!xir_compile_context_valid(context) || !output) return XR_XIR_BAD_STRUCTURE;

    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (!node || (node->kind!=XR_XIR_TYPE_SLICE && node->kind!=XR_XIR_TYPE_MUT_SLICE &&
        node->kind!=XR_XIR_TYPE_TUPLE && node->kind!=XR_XIR_TYPE_NULLABLE)) {*output=0;return XR_XIR_OK;}
    uint8_t *flags=NULL;
    XrXirStatus status=view_flags_build(context,types,&flags);
    if (status==XR_XIR_OK) *output=view_flags_at(types,flags,type);
    xr_compile_resources_free(flags);return status;
}

XR_FUNC XrXirStatus xr_xir_compile_view_return_candidate(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType callable,uint32_t *output) {
    if (!xir_compile_context_valid(context) || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *node=xr_xir_callable_signature(types,callable);
    if (!node) return XR_XIR_BAD_TYPE;
    uint8_t *flags=NULL;
    XrXirStatus status=view_flags_build(context,types,&flags);
    if (status==XR_XIR_OK) status=view_return_candidate(types,flags,node,output);
    xr_compile_resources_free(flags);return status;
}

#include "xxir_view_origins.inc.c"
#include "xxir_view_liveness_internal.h"
#include "xxir_view_roots.inc.c"
#include "xxir_buffer_scoped_boundary.inc.c"

static bool view_boundary_supported(const XrXirTypes *types,const uint8_t *flags,XrXirType type) {
    if (!view_flags_at(types,flags,type)) return true;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (node && node->kind==XR_XIR_TYPE_NULLABLE && view_pointer(types,node->element))
        node=xr_xir_type_node(types,node->element);
    if (node && node->kind==XR_XIR_TYPE_SLICE && node->element==XR_XIR_I64) return true;
    return node && (node->kind==XR_XIR_TYPE_SLICE || node->kind==XR_XIR_TYPE_PTR ||
        node->kind==XR_XIR_TYPE_MUT_PTR) && node->element==XR_XIR_U8;
}
XR_FUNC XrXirStatus xir_compile_view_module_boundary_input(const XrXirCompileContext *context,
    XirEffectInput input,XrXirDiagnostic *diagnostic) {
    const XrXirModule *module=input.module;
    if(!input.construction)return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_context_valid(context) || !module) return XR_XIR_BAD_STRUCTURE;
    uint8_t *flags=NULL;
    XrXirStatus status=view_flags_build_mode(context,module->types,&flags,true);
    if (status!=XR_XIR_OK || !flags) return status;
    status=view_origins_verify(context,module,input.construction,flags,diagnostic,input.control);
    if (status==XR_XIR_OK) status=xir_compile_view_roots_verified(context,(XirEffectInput){module,input.construction,input.control},NULL,NULL,diagnostic);
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {
        const XrXirFunction *body=&module->functions[f];
        bool callback=false;
        status=xir_compile_buffer_scoped_callback(context,input,f,&callback);
        if(status!=XR_XIR_OK)break;
        bool begin=module->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN;
        if(begin){status=xir_native_callable_verify(module,f);if(status!=XR_XIR_OK)break;}
        if (!view_boundary_supported(module->types,flags,body->result)&&!begin) {
            status=XR_XIR_BAD_TYPE;
            if (diagnostic) *diagnostic=(XrXirDiagnostic){status,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
            break;
        }
        for (uint32_t p=0;p<body->parameter_count && status==XR_XIR_OK;++p) {
            if (!view_boundary_supported(module->types,flags,body->parameters[p])&&
                !(callback&&p+1u==body->parameter_count)) {
                status=XR_XIR_BAD_TYPE;
                if(diagnostic)*diagnostic=(XrXirDiagnostic){status,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
            }
        }
        for (uint32_t i=0;i<body->instruction_count && status==XR_XIR_OK;++i) {
            const XrXirInstruction *op=body->instructions+i;
            if(xr_xir_op_references_function(op->op) && op->immediate>=0 &&
                (uint64_t)op->immediate<module->function_count &&
                module->declarations->functions[op->immediate].native_callable==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN) {
                bool scoped=false;
                status=xir_compile_buffer_scoped_site(context,input,f,i,&scoped);
                if(status==XR_XIR_OK&&!scoped)status=XR_XIR_BAD_TYPE;
                if(status!=XR_XIR_OK)break;
            }
            if (!view_boundary_supported(module->types,flags,body->instructions[i].type)) {
                const XrXirTypeNode *temporary=xr_xir_type_node(module->types,body->instructions[i].type);
                bool local=temporary&&((temporary->kind==XR_XIR_TYPE_SLICE&&temporary->element==XR_XIR_I64)||
                    (temporary->kind==XR_XIR_TYPE_MUT_SLICE&&
                     (temporary->element==XR_XIR_U8||temporary->element==XR_XIR_I64)));
                if (!local)
                    status=XR_XIR_BAD_TYPE;
            }
        }
    }
    xr_compile_resources_free(flags);

    return status;
}
