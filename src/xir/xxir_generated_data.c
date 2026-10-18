/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generated_data.c - Bounded ordinary value decoding for generated bodies
 *
 * KEY CONCEPT:
 *   Public owning Value operations observe flat rows on the original ledger.
 *   Every decoded reference remains unchecked until ordinary Built receiving.
 */
#include "xxir_generated_data.h"
#include "xxir_compile_memory.h"
#include "xxir_compile_domain.h"
#include "xxir_type_arena.h"
#include "xxir_types.h"
#include "xxir_tuple.h"
#include "xxir_array.h"
#include "../shared/xr_utf8_core.h"

struct XrXirGeneratedData {
    XrXirGeneratedDataView view;
    XrXirFunction *functions;
    XrXirGeneric *generics;
    XrXirType *parameters,*arguments;
    XrXirBlock *blocks;
    XrXirInstruction *instructions;
    uint32_t *operands,*members,*roots;
    int32_t *fields;
    XrXirLiteral *literals;
};
typedef struct GeneratedDataReader {
    const XrXirCompileContext *context;
    const XrXirGeneratedDataRequest *request;
    XrXirGeneratedData *owner;
    XrXirDomain *domain;
    XrXirValue tables[7];
    uint32_t counts[7];
    uint32_t used[5];
    XrXirStatus status;
} GeneratedDataReader;

static bool data_work(GeneratedDataReader *r,uint64_t work) {
    if (r->status!=XR_XIR_OK) return false;
    if (!xir_compile_work(r->context,work)) r->status=XR_XIR_BUDGET;
    return r->status==XR_XIR_OK;
}
static bool data_bad(GeneratedDataReader *r) {
    if (r->status==XR_XIR_OK) r->status=XR_XIR_BAD_STRUCTURE;
    return false;
}
static bool data_value_status(GeneratedDataReader *r,XrXirValueStatus status) {
    if (status==XR_XIR_VALUE_OK) return true;
    r->status=status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    return false;
}
static void *data_alloc(GeneratedDataReader *r,uint32_t count,size_t size) {
    if (!count || r->status!=XR_XIR_OK) return NULL;
    return xir_compile_calloc(r->context,count,size,&r->status);
}
static bool data_tuple(GeneratedDataReader *r,const XrXirValue *value,uint32_t count) {
    if (!data_work(r,(uint64_t)count*16+32)) return false;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(value));
    const XrXirTypeNode *node=xr_xir_tuple_signature(types,(XrXirType)value->type);
    return (node && !node->parameter_span && node->parameter_count==count) || data_bad(r);
}
static bool data_field(GeneratedDataReader *r,const XrXirValue *value,uint32_t field,
    XrXirValue *output) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(value));
    const XrXirTypeNode *node=xr_xir_tuple_signature(types,(XrXirType)value->type);
    if (!node || !data_work(r,(uint64_t)node->parameter_count*16+32)) return data_bad(r);
    return data_value_status(r,xr_xir_tuple_get(value,field,output));
}
static bool data_integer(GeneratedDataReader *r,const XrXirValue *value,int64_t *output) {
    if (!data_work(r,1)) return false;
    if (value->type!=XR_XIR_I64 || value->reserved) return data_bad(r);
    *output=value->payload;return true;
}
static bool data_number(GeneratedDataReader *r,const XrXirValue *row,uint32_t field,int64_t *output) {
    XrXirValue value={0};
    if (!data_field(r,row,field,&value)) return false;
    bool ok=data_integer(r,&value,output);xr_xir_value_drop(&value);return ok;
}
static bool data_unsigned(GeneratedDataReader *r,int64_t value,uint32_t *output) {
    if (value<0 || (uint64_t)value>UINT32_MAX) return data_bad(r);
    *output=(uint32_t)value;return true;
}
static bool data_row(GeneratedDataReader *r,uint32_t table,uint32_t index,XrXirValue *output) {
    if (table>=7 || index>=r->counts[table] || !data_work(r,32)) return data_bad(r);
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&r->tables[table]),
        .domain=r->domain,.work=UINT64_MAX,.scratch_bytes=r->context->limits.frame_bytes};
    XrXirFaultDetail fault={0};
    return data_value_status(r,xr_xir_array_get(&r->tables[table],index,&admission,output,&fault));
}
static bool data_schema_row(GeneratedDataReader *r,const XrXirTypes *types,XrXirType type,
    uint32_t columns,bool name) {
    const XrXirTypeNode *row=xr_xir_tuple_signature(types,type);
    if (!row || row->parameter_span || row->parameter_count!=columns || !row->parameters)
        return data_bad(r);
    for (uint32_t c=0;c<columns;++c) {
        if (!data_work(r,1)) return false;
        if (row->parameters[c].mode || row->parameters[c].type!=(name && !c?XR_XIR_STRING:XR_XIR_I64))
            return data_bad(r);
    }
    return true;
}
static bool data_tables(GeneratedDataReader *r) {
    const XrXirValue *value=r->request->value;
    if (!data_tuple(r,value,8)) return false;
    int64_t version=0;
    if (!data_number(r,value,0,&version) || version!=1) return data_bad(r);
    const uint32_t columns[]={14,0,4,10,0,0,0};
    for (uint32_t t=0;t<7;++t) {
        if (!data_field(r,value,t+1,&r->tables[t])) return false;
        const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(&r->tables[t]));
        const XrXirTypeNode *array=xr_xir_type_node(types,(XrXirType)r->tables[t].type);
        if (!array || array->kind!=XR_XIR_TYPE_ARRAY || array->parameter_span) return data_bad(r);
        if (columns[t]) {
            if (!data_schema_row(r,types,array->element,columns[t],t==0)) return false;
        } else if (array->element!=(t==5?XR_XIR_STRING:XR_XIR_I64)) return data_bad(r);
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&r->tables[t]),
            .domain=r->domain,.work=UINT64_MAX,.scratch_bytes=r->context->limits.frame_bytes};
        int64_t length=0;
        if (!data_work(r,32) || !data_value_status(r,xr_xir_array_len(&r->tables[t],&admission,&length)) ||
            !data_unsigned(r,length,&r->counts[t])) return false;
    }
    if (r->request->first_function>r->context->limits.functions ||
        r->counts[0]>r->context->limits.functions-r->request->first_function ||
        r->counts[2]>r->context->limits.blocks || r->counts[3]>r->context->limits.instructions ||
        r->counts[5]>UINT32_MAX-r->request->first_literal) {
        r->status=XR_XIR_BUDGET;return false;
    }
    return true;
}
static bool data_type(GeneratedDataReader *r,int64_t reference,XrXirType *output) {
    XrXirType type=XR_XIR_UNIT;
    if (reference>=0) {
        if ((uint64_t)reference>=r->request->reference_count) return data_bad(r);
        type=r->request->references[(uint32_t)reference];
    } else {
        if (reference<-(int64_t)XR_XIR_CONSTRUCTED_TYPE_BASE) return data_bad(r);
        type=(XrXirType)(-reference-1);
    }
    if ((uint32_t)type<XR_XIR_CONSTRUCTED_TYPE_BASE) {
        XrXirTypeDescription description={0};
        XrXirStatus status=xr_xir_compile_type_description(r->context,NULL,type,&description);
        if (status!=XR_XIR_OK) { r->status=status;return false; }
    }
    if ((uint32_t)type>=XR_XIR_CONSTRUCTED_TYPE_BASE &&
        !xr_xir_type_node(r->request->types,type) &&
        ((uint32_t)type<XR_XIR_TYPE_PARAMETER_BASE || (uint32_t)type>=XR_XIR_TYPE_PARAMETER_LIMIT))
        return data_bad(r);
    *output=type;return true;
}
static bool data_allocate_tables(GeneratedDataReader *r) {
    XrXirGeneratedData *o=r->owner;
    o->view.function_count=r->counts[0];o->view.instruction_count=r->counts[3];o->view.literal_count=r->counts[5];
    o->functions=data_alloc(r,r->counts[0],sizeof(*o->functions));
    o->generics=data_alloc(r,r->counts[0],sizeof(*o->generics));
    o->members=data_alloc(r,r->counts[0],sizeof(*o->members));o->roots=data_alloc(r,r->counts[0],sizeof(*o->roots));
    o->parameters=data_alloc(r,r->counts[1],sizeof(*o->parameters));o->blocks=data_alloc(r,r->counts[2],sizeof(*o->blocks));
    o->instructions=data_alloc(r,r->counts[3],sizeof(*o->instructions));o->fields=data_alloc(r,r->counts[3],sizeof(*o->fields));
    o->operands=data_alloc(r,r->counts[4],sizeof(*o->operands));o->literals=data_alloc(r,r->counts[5],sizeof(*o->literals));
    o->arguments=data_alloc(r,r->counts[6],sizeof(*o->arguments));
    return r->status==XR_XIR_OK;
}
static bool data_values(GeneratedDataReader *r,uint32_t table) {
    for (uint32_t n=0;n<r->counts[table];++n) {
        XrXirValue value={0};int64_t number=0;
        if (!data_row(r,table,n,&value)) return false;
        bool ok=data_integer(r,&value,&number);xr_xir_value_drop(&value);
        if (!ok) return false;
        if (table==4) { if (!data_unsigned(r,number,&r->owner->operands[n])) return false; }
        else if (!data_type(r,number,table==1?&r->owner->parameters[n]:&r->owner->arguments[n])) return false;
    }
    return true;
}
static bool data_blocks(GeneratedDataReader *r) {
    for (uint32_t n=0;n<r->counts[2];++n) {
        XrXirValue row={0};int64_t values[4]={0};bool ok=data_row(r,2,n,&row);
        for (uint32_t c=0;ok && c<4;++c) ok=data_number(r,&row,c,&values[c]);
        xr_xir_value_drop(&row);if (!ok) return false;
        XrXirBlock *block=&r->owner->blocks[n];
        if (!data_unsigned(r,values[0],&block->first) || !data_unsigned(r,values[1],&block->count) ||
            !data_unsigned(r,values[2],&block->panic) || !data_unsigned(r,values[3],&block->frontier)) return false;
    }
    return true;
}
static bool data_immediate(GeneratedDataReader *r,XrXirInstruction *op) {
    if (op->op==XR_XIR_CONST_STRING) {
        if (op->immediate<0 || (uint64_t)op->immediate>=r->counts[5]) return data_bad(r);
        op->immediate+=r->request->first_literal;
    }
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE &&
        op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_CLEANUP_REGISTER) return true;
    if (op->immediate>=0) {
        if ((uint64_t)op->immediate>=r->counts[0]) return data_bad(r);
        op->immediate+=r->request->first_function;
    } else {
        if (op->immediate==INT64_MIN || (uint64_t)(-op->immediate-1)>=r->request->callee_count)
            return data_bad(r);
        op->immediate=r->request->callees[(uint32_t)(-op->immediate-1)];
    }
    return true;
}
static bool data_instructions(GeneratedDataReader *r) {
    for (uint32_t n=0;n<r->counts[3];++n) {
        XrXirValue row={0};int64_t v[10]={0};bool ok=data_row(r,3,n,&row);
        for (uint32_t c=0;ok && c<10;++c) ok=data_number(r,&row,c,&v[c]);
        xr_xir_value_drop(&row);if (!ok) return false;
        if (v[0]<0 || v[0]>=XR_XIR_OP_COUNT || v[9]<-1 ||
            (v[9]>=0 && (uint64_t)v[9]>=r->request->field_count) || v[9]>INT32_MAX)
            return data_bad(r);
        XrXirInstruction *op=&r->owner->instructions[n];op->op=(XrXirOp)v[0];op->immediate=v[6];
        r->owner->fields[n]=(int32_t)v[9];
        if (!data_type(r,v[1],&op->type) || !data_unsigned(r,v[2],&op->args[0]) ||
            !data_unsigned(r,v[3],&op->args[1]) || !data_unsigned(r,v[4],&op->targets[0]) ||
            !data_unsigned(r,v[5],&op->targets[1]) || !data_unsigned(r,v[7],&op->type_arguments[0]) ||
            !data_unsigned(r,v[8],&op->type_arguments[1]) || !data_immediate(r,op)) return false;
    }
    return true;
}
static int data_text_read(void *context,const uint8_t *address,uint8_t *output) {
    GeneratedDataReader *r=context;if (!data_work(r,1)) return 0;*output=*address;return 1;
}
static bool data_text(GeneratedDataReader *r,const XrXirValue *value,bool name,XrXirLiteral *output) {
    const char *bytes=NULL;size_t length=0;
    if (!data_work(r,32) || !xr_xir_string_view(value,&bytes,&length) || length>UINT32_MAX ||
        length==SIZE_MAX || (name && !length)) return data_bad(r);
    if (name) {
        XrUtf8ScanResult scan={0};
        if (!xr_utf8_core_scan_strict_read((const uint8_t *)bytes,length,data_text_read,r,&scan))
            return false;
        if (scan.error!=XR_UTF8_OK) return data_bad(r);
        for (size_t n=0;n<length;++n) {
            if (!data_work(r,1)) return false;
            if (!bytes[n]) return data_bad(r);
        }
    }
    if (!data_work(r,length)) return false;
    char *owned=xir_compile_calloc(r->context,length+1,1,&r->status);
    if (!owned) return false;
    if (length) memcpy(owned,bytes,length);
    *output=(XrXirLiteral){owned,(uint32_t)length};return true;
}
static bool data_literals(GeneratedDataReader *r) {
    for (uint32_t n=0;n<r->counts[5];++n) {
        XrXirValue value={0};if (!data_row(r,5,n,&value)) return false;
        bool ok=data_text(r,&value,false,&r->owner->literals[n]);xr_xir_value_drop(&value);
        if (!ok) return false;
    }
    return true;
}
typedef struct GeneratedDataSpan { uint32_t first,count; } GeneratedDataSpan;
static bool data_span(GeneratedDataReader *r,const int64_t *values,uint32_t family,
    GeneratedDataSpan *span) {
    const uint32_t tables[]={1,2,3,4,6};uint32_t table=tables[family];
    if (!data_unsigned(r,values[0],&span->first) || !data_unsigned(r,values[1],&span->count)) return false;
    if (span->first!=r->used[family] || span->first>r->counts[table] || span->count>r->counts[table]-span->first)
        return data_bad(r);
    r->used[family]+=span->count;return true;
}
static bool data_function(GeneratedDataReader *r,uint32_t n) {
    XrXirValue row={0},name={0};int64_t v[13]={0};
    bool ok=data_row(r,0,n,&row) && data_field(r,&row,0,&name);
    XrXirLiteral text={0};if (ok) ok=data_text(r,&name,true,&text);
    xr_xir_value_drop(&name);
    if (ok) { r->owner->functions[n].name=text.bytes;r->owner->functions[n].name_length=text.length; }
    for (uint32_t c=0;ok && c<13;++c) ok=data_number(r,&row,c+1,&v[c]);
    xr_xir_value_drop(&row);if (!ok) return false;
    if (v[0]<-1 || (v[0]>=0 && (uint64_t)v[0]>=r->request->method_count) ||
        !data_unsigned(r,v[1],&r->owner->roots[n]) || r->owner->roots[n]>=r->counts[0]) return data_bad(r);
    r->owner->members[n]=v[0]<0?UINT32_MAX:(uint32_t)v[0];
    XrXirFunction *f=&r->owner->functions[n];
    if (!data_type(r,v[2],&f->result)) return false;
    GeneratedDataSpan spans[5]={0};
    for (uint32_t t=0;t<5;++t) if (!data_span(r,&v[3+t*2],t,&spans[t])) return false;
    if (spans[0].count>r->context->limits.parameters) { r->status=XR_XIR_BUDGET;return false; }
    f->parameter_count=spans[0].count;f->block_count=spans[1].count;
    f->instruction_count=spans[2].count;f->operand_count=spans[3].count;
    f->parameters=f->parameter_count?r->owner->parameters+spans[0].first:NULL;
    f->blocks=f->block_count?r->owner->blocks+spans[1].first:NULL;
    f->instructions=f->instruction_count?r->owner->instructions+spans[2].first:NULL;
    f->operands=f->operand_count?r->owner->operands+spans[3].first:NULL;
    r->owner->generics[n].arguments=spans[4].count?r->owner->arguments+spans[4].first:NULL;
    r->owner->generics[n].argument_count=spans[4].count;return true;
}
static bool data_functions(GeneratedDataReader *r) {
    for (uint32_t n=0;n<r->counts[0];++n) if (!data_function(r,n)) return false;
    const uint32_t families[]={1,2,3,4,6};
    for (uint32_t t=0;t<5;++t) if (r->used[t]!=r->counts[families[t]]) return data_bad(r);
    for (uint32_t n=0;n<r->counts[0];++n) {
        uint32_t root=r->owner->roots[n];
        if (!data_work(r,1)) return false;
        if (r->owner->members[root]==UINT32_MAX || r->owner->roots[root]!=root ||
            (r->owner->members[n]!=UINT32_MAX && root!=n)) return data_bad(r);
    }
    return true;
}
XR_FUNC void xr_xir_compile_generated_data_free(XrXirGeneratedData *o) {
    if (!o) return;
    for (uint32_t f=0;o->functions && f<o->view.function_count;++f)
        xr_compile_resources_free((void *)o->functions[f].name);
    for (uint32_t n=0;o->literals && n<o->view.literal_count;++n)
        xr_compile_resources_free((void *)o->literals[n].bytes);
    xr_compile_resources_free(o->functions);xr_compile_resources_free(o->generics);
    xr_compile_resources_free(o->members);xr_compile_resources_free(o->roots);
    xr_compile_resources_free(o->parameters);xr_compile_resources_free(o->blocks);
    xr_compile_resources_free(o->instructions);xr_compile_resources_free(o->fields);
    xr_compile_resources_free(o->operands);xr_compile_resources_free(o->literals);
    xr_compile_resources_free(o->arguments);xr_compile_resources_free(o);
}
XR_FUNC const XrXirGeneratedDataView *xr_xir_compile_generated_data_view(const XrXirGeneratedData *o) {
    return o?&o->view:NULL;
}
XR_FUNC XrXirStatus xr_xir_compile_generated_data_decode(const XrXirCompileContext *context,
    const XrXirGeneratedDataRequest *request,XrXirGeneratedData **output) {
    if (!xir_compile_context_valid(context) || !request || !request->value || !output || *output ||
        (request->reference_count && !request->references) || (request->callee_count && !request->callees))
        return XR_XIR_BAD_STRUCTURE;
    GeneratedDataReader r={.context=context,.request=request};
    XrXirStatus status=XR_XIR_OK;r.owner=xir_compile_calloc(context,1,sizeof(*r.owner),&status);
    if (!r.owner) return status;
    bool ok=data_value_status(&r,xr_xir_compile_domain_new(context->resources,UINT64_MAX,&r.domain)) &&
        data_tables(&r) && data_allocate_tables(&r) && data_values(&r,1) && data_values(&r,4) &&
        data_values(&r,6) && data_blocks(&r) && data_instructions(&r) && data_literals(&r) && data_functions(&r);
    for (uint32_t t=0;t<7;++t) xr_xir_value_drop(&r.tables[t]);
    xr_xir_domain_drop(r.domain);
    if (!ok) {
        status=r.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:r.status;
        xr_xir_compile_generated_data_free(r.owner);return status;
    }
    XrXirGeneratedData *o=r.owner;
    o->view.functions=o->functions;o->view.generics=o->generics;
    o->view.members=o->members;o->view.roots=o->roots;o->view.instruction_fields=o->fields;
    o->view.literals=o->literals;*output=o;return XR_XIR_OK;
}
