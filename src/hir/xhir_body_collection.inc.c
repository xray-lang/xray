/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Collection uses own their operands and complete Map provenance until seal. */
#include "../xir/xxir_constant_key.h"
typedef struct HirBodyCollection {
    uint32_t count,ordinal;
    XrXirMapLiteral literal;
    XrHirCollectionValue *values;
} HirBodyCollection;
_Static_assert(sizeof(HirBodyCollection)%_Alignof(XrHirCollectionValue)==0 &&
    _Alignof(XrHirCollectionValue)>=_Alignof(XrXirMapLiteralOrigin),"collection snapshot alignment");

static XrXirStatus body_collection_provenance(const XrHirCollectionConstructInput *input,
    XrCompileRequestControl *control) {
    const XrXirMapLiteral *literal=input->provenance;
    if(input->literal.kind!=XR_HIR_COLLECTION_MAP)
        return !literal && !input->ordinal?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(!literal || literal->module!=input->origin.module || literal->count!=input->literal.count ||
        (literal->count!=0)!=(literal->items!=NULL) ||
        (literal->file.length!=0)!=(literal->file.bytes!=NULL))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_constant_key_utf8((XrXirConstantKey){.kind=XR_XIR_KEY_STRING,.string=literal->file},control);
    if(status!=XR_XIR_OK)return status;
    for(size_t offset=0;offset<literal->file.length;) {
        status=body_poll(control);if(status!=XR_XIR_OK)return status;
        size_t count=literal->file.length-offset;if(count>4096)count=4096;
        if(memchr(literal->file.bytes+offset,0,count))return XR_XIR_BAD_STRUCTURE;
        offset+=count;
    }
    for(uint32_t n=0;n<literal->count;++n) {
        if(!(n%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrXirMapLiteralOrigin origin=literal->items[n];
        if(origin.kind!=(input->literal.entries[n].spread?1u:0u) || !origin.line || !origin.column)
            return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_body_collection_construct(XrHirBody *body,const XrHirCollectionConstructInput *input,
    XrCompileRequestControl *control,XrHirBodyUse *out,XrHirCollectionError *error) {
    if(!body || body->ended || !input || input->types!=body->types || !out || !error ||
        !input->compile || input->compile->resources!=body->resources || (input->count && !input->values) ||
        body_tuple_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrHirCollection checked={0};XrHirCollectionError problem={0};
    XrXirStatus status=xr_hir_collection_check(input->compile,body->types,&input->literal,control,&checked,&problem);
    if(status!=XR_XIR_OK){if(status==XR_XIR_BAD_TYPE)*error=problem;return status;}
    const XrXirTypeNode *type=xr_xir_type_node(body->types,input->result);
    bool map=checked.kind==XR_HIR_COLLECTION_MAP;
    uint32_t kind=map?XR_XIR_TYPE_MAP:XR_XIR_TYPE_ARRAY;
    if(!type || type->kind!=kind ||
        type->element!=(map?checked.key:checked.value) || (map && type->result!=checked.value) ||
        input->count!=checked.operands)return XR_XIR_BAD_TYPE;
    if(map && !input->provenance)return XR_XIR_BAD_STRUCTURE;
    XrXirMapLiteral literal=map?*input->provenance:(XrXirMapLiteral){0};
    uint64_t total=sizeof(HirBodyCollection)+(uint64_t)input->count*sizeof(XrHirCollectionValue)+
        (uint64_t)literal.count*sizeof(XrXirMapLiteralOrigin)+literal.file.length;
    if(total>SIZE_MAX || total>input->compile->limits.frame_bytes)return XR_XIR_BUDGET;
    status=body_collection_provenance(input,control);if(status!=XR_XIR_OK)return status;
    uint32_t at=0;
    for(uint32_t n=0;n<input->literal.count;++n) {
        if(!(n%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrHirCollectionEntry entry=input->literal.entries[n];
        if(!map && entry.spread)return XR_XIR_BAD_STRUCTURE;
        if(map && !entry.spread) {
            if(input->values[at].type!=entry.key || input->values[at].value==UINT32_MAX)return XR_XIR_BAD_TYPE;
            ++at;
        }
        if(input->values[at].type!=entry.value || input->values[at].value==UINT32_MAX)return XR_XIR_BAD_TYPE;
        ++at;
    }
    if(at!=input->count)return XR_XIR_BAD_STRUCTURE;
    HirBodyCollection *owned=NULL;status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)total,(void **)&owned));
    if(status!=XR_XIR_OK)return status;
    *owned=(HirBodyCollection){.count=input->count,.ordinal=input->ordinal,.literal=literal};
    owned->values=(XrHirCollectionValue *)(owned+1);
    XrXirMapLiteralOrigin *origins=(XrXirMapLiteralOrigin *)(owned->values+input->count);
    char *file=(char *)(origins+literal.count);
    owned->literal.items=literal.count?origins:NULL;owned->literal.file.bytes=literal.file.length?file:NULL;
    status=body_bytes(owned->values,input->values,(size_t)input->count*sizeof(*input->values),control);
    if(status==XR_XIR_OK)status=body_bytes(origins,literal.items,(size_t)literal.count*sizeof(*origins),control);
    if(status==XR_XIR_OK)status=body_bytes(file,literal.file.bytes,literal.file.length,control);
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.target=input->result};XrHirBodyUse use={0};
    if(status==XR_XIR_OK)status=xr_hir_body_check(body,body->types,&typed,control,&use,NULL);
    if(status!=XR_XIR_OK){xr_compile_resources_free(owned);return status;}
    HirBodyUse *row=&body->uses[use.index];row->collection=owned;row->required_instruction=true;
    row->action.operation=map?XR_XIR_MAP_LITERAL:XR_XIR_ARRAY_NEW;row->action.argument_count=input->count;
    *out=use;*error=(XrHirCollectionError){0};return XR_XIR_OK;
}
static XrXirStatus body_collection_map_project(const HirBodyCollection *owned,
    const XrXirMapLiteral *literal,XrCompileRequestControl *control) {
    XrXirMapLiteral expected=owned->literal;
    if(!literal || literal->module!=expected.module || literal->count!=expected.count ||
        literal->file.length!=expected.file.length || (!!literal->file.bytes!=!!expected.file.bytes) ||
        (!!literal->items!=!!expected.items))return XR_XIR_BAD_STRUCTURE;
    int order=0;XrXirStatus status=xir_receive_compare(control,literal->file.bytes,expected.file.bytes,expected.file.length,&order);
    if(status!=XR_XIR_OK)return status;
    if(order)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t n=0;n<literal->count;++n) {
        if(!(n%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrXirMapLiteralOrigin a=literal->items[n],b=expected.items[n];
        if(a.kind!=b.kind || a.line!=b.line || a.column!=b.column)return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
static XrXirStatus body_collection_project(const XrHirBody *body,const HirBodyUse *row,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    if(!projection->has_instruction || !body->declarations.functions || !*body->declarations.functions ||
        projection->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&(*body->declarations.functions)[projection->function];
    if(!function->instructions || projection->instruction>=function->instruction_count)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction actual=function->instructions[projection->instruction];const HirBodyCollection *owned=row->collection;
    bool map=row->action.operation==XR_XIR_MAP_LITERAL;
    if(actual.op!=row->action.operation || actual.type!=row->action.target || actual.args[1]!=owned->count ||
        projection->source_argument_count!=owned->count || actual.args[0]>function->operand_count ||
        actual.args[1]>function->operand_count-actual.args[0] || actual.immediate!=(map?(int64_t)owned->ordinal:0) ||
        actual.targets[0] || actual.targets[1] || actual.type_arguments[0] || actual.type_arguments[1] ||
        (owned->count && (!function->operands || !projection->source_arguments)))return XR_XIR_BAD_STRUCTURE;
    if(map) {
        const XrXirDeclarations *d=projection->collection_declarations;
        if(!d || !d->map_literals || owned->ordinal>=d->map_literal_count ||
            !body->declarations.identities || d->functions!=*body->declarations.identities ||
            !d->functions || owned->literal.module>=d->module_count ||
            d->functions[projection->function].module!=owned->literal.module)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_collection_map_project(owned,d->map_literals+owned->ordinal,control);if(status!=XR_XIR_OK)return status;
    } else if(projection->collection_declarations)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t n=0;n<owned->count;++n) {
        if(!(n%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrHirCollectionValue value=owned->values[n];uint32_t target=function->operands[actual.args[0]+n];
        if(value.value>=projection->source_value_count || !projection->source_value_map ||
            projection->source_arguments[n]!=value.value || projection->source_value_map[value.value]!=target ||
            !body_tuple_value_type(function,target,value.type))return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
