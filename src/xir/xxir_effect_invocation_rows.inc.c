/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_rows.inc.c - Physical data for one immutable body request
 *
 * KEY CONCEPT:
 *   Only actual types, instructions and write ordinals are shared. Every
 *   invocation retains independent input, origin, Class and effect equations.
 */
typedef struct EffectInvocationRowData {
    void *memory;
    XrXirType *types;
    XrXirInstruction *instructions;
    const XrXirInstruction **instruction_refs;
    uint32_t *write_heads,*write_next;
    uint32_t values,parameters,instruction_count,instruction_storage_count;
} EffectInvocationRowData;

struct EffectInvocationRows {
    const EffectBodyMaterial *material;
    XrCompileResources *resources;
    EffectInvocationRowData *functions;
    uint32_t count;
};

static void effect_invocation_rows_drop(EffectInvocationRows *rows) {
    if (!rows) return;
    for (uint32_t f=0;rows->functions && f<rows->count;++f)
        xr_compile_resources_free(rows->functions[f].memory);
    xr_compile_resources_free(rows->functions);*rows=(EffectInvocationRows){0};
}

static XrXirStatus effect_invocation_rows_open(EffectInvocationFlow *flow,
    EffectInvocationRows *rows) {
    const XrXirCompileContext *work=flow->owner->work;
    const EffectBodyMaterial *material=flow->body.owner;
    XrXirStatus status=effect_body_request_sequence_binding(&flow->body);
    if (status!=XR_XIR_OK) return status;
    if (material!=flow->owner->final.material) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (rows->functions) {
        return rows->material==material && rows->resources==work->resources &&
            rows->count==effect_body_count(material)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (rows->material || rows->resources || rows->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(material);
    if ((uint64_t)count>SIZE_MAX/sizeof(*rows->functions)) return XR_XIR_BUDGET;
    EffectInvocationRowData *functions=count?xir_compile_calloc(work,count,sizeof(*functions),&status):NULL;
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(*rows))) {
        xr_compile_resources_free(functions);return XR_XIR_BUDGET;
    }
    *rows=(EffectInvocationRows){material,work->resources,functions,count};return XR_XIR_OK;
}

/* This is only allocation sizing. The original receiver still checks every
 * current canonical run, selected delta, neighbor and EOF before any use. */
static XrXirStatus effect_invocation_rows_expanded(EffectInvocationFlow *flow,uint32_t *output) {
    uint32_t f=flow->owner->final.nodes[flow->node].body;
    const EffectBodyMaterial *material=flow->body.owner;
    if (!output || material!=flow->owner->final.material || f>=effect_body_count(material) ||
        !material->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&material->functions[f];
    if (body->instruction_count!=flow->function->instruction_count ||
        body->run_count>body->run_capacity || body->run_count>body->instruction_count ||
        (body->run_count && !body->runs)) return XR_XIR_BAD_STRUCTURE;
    uint64_t expanded=0;
    for (uint32_t r=0;r<body->run_count;++r) {
        if (!xir_compile_work(flow->owner->work,6)) return XR_XIR_BUDGET;
        const EffectBodyRun *run=&body->runs[r];
        if (!run->count || run->first>=body->instruction_count ||
            run->count>body->instruction_count-run->first) return XR_XIR_BAD_STRUCTURE;
        if (run->mask && !(run->mask&EFFECT_BODY_LITERAL_PAIR)) expanded+=run->count-1;
        if (expanded>body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    }
    *output=(uint32_t)expanded;return XR_XIR_OK;
}

/* Complete literal and constant rows already have this request's retained
 * material lifetime. Only varying later rows need another owned value. The
 * reference directory and expanded values share this one data allocation. */
static XrXirStatus effect_invocation_rows_build(EffectInvocationFlow *flow,
    EffectInvocationRowData *output) {
    const XrXirCompileContext *work=flow->owner->work;
    uint32_t expanded=0;XrXirStatus status=effect_invocation_rows_expanded(flow,&expanded);
    if (status!=XR_XIR_OK) return status;
    uint64_t types=(uint64_t)flow->values*sizeof(*flow->types);
    if (types>SIZE_MAX-7) return XR_XIR_BUDGET;
    uint64_t instructions=(types+7)&~UINT64_C(7);
    uint64_t operations=(uint64_t)expanded*sizeof(*flow->instructions);
    uint64_t references=(uint64_t)flow->function->instruction_count*sizeof(*flow->instruction_refs);
    if (references>SIZE_MAX-operations) return XR_XIR_BUDGET;
    uint64_t rows=operations+references;
    uint64_t indices=((uint64_t)flow->values+flow->function->instruction_count)*sizeof(uint32_t);
    if (rows>SIZE_MAX-instructions || indices>SIZE_MAX-instructions-rows)
        return XR_XIR_BUDGET;
    size_t bytes=(size_t)(instructions+rows+indices);
    void *memory=bytes?xir_compile_alloc(work,bytes,&status):NULL;
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(EffectInvocationRowData)+4*sizeof(void *))) {
        xr_compile_resources_free(memory);return XR_XIR_BUDGET;
    }
    EffectInvocationRowData data={.memory=memory,.values=flow->values,
        .parameters=flow->function->parameter_count,.instruction_count=flow->function->instruction_count,
        .instruction_storage_count=expanded};
    if (flow->values) {
        data.types=memory;
        data.write_heads=(uint32_t *)((uint8_t *)memory+(size_t)(instructions+rows));
    }
    if (flow->function->instruction_count) {
        data.instructions=expanded?(XrXirInstruction *)((uint8_t *)memory+(size_t)instructions):NULL;
        data.instruction_refs=(const XrXirInstruction **)((uint8_t *)memory+(size_t)(instructions+operations));
        data.write_next=(uint32_t *)((uint8_t *)memory+(size_t)(instructions+rows)+
            (size_t)flow->values*sizeof(uint32_t));
    }
    flow->types=data.types;flow->instructions=data.instructions;
    flow->instruction_refs=data.instruction_refs;flow->instruction_storage_count=expanded;
    flow->write_heads=data.write_heads;flow->write_next=data.write_next;
    status=effect_invocation_flow_types(flow);
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*output))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        xr_compile_resources_free(memory);flow->types=NULL;flow->instructions=NULL;
        flow->instruction_refs=NULL;flow->instruction_storage_count=0;
        flow->write_heads=NULL;flow->write_next=NULL;flow->types_ready=false;return status;
    }
    *output=data;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_flow_rows(EffectInvocationFlow *flow) {
    if (!flow || !flow->owner || !flow->function || flow->types_ready ||
        flow->node>=flow->owner->final.count || !xir_compile_context_valid(flow->owner->work) ||
        !flow->body.owner || !flow->body.head || !flow->body.instruction || !flow->body.type ||
        !flow->body.shape || !flow->body.domain.types || !flow->body.read.owner || !flow->body.read.consume)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationRows *rows=flow->owner->physical_rows;
    if (!rows || !effect_body_request_sequence_owned(&flow->body))
        return effect_invocation_flow_types(flow);
    XrXirStatus status=effect_invocation_rows_open(flow,rows);
    if (status!=XR_XIR_OK) return status;
    uint32_t body=flow->owner->final.nodes[flow->node].body;
    if (body>=rows->count || !rows->functions) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationRowData *data=&rows->functions[body];
    if (!data->memory) {
        return effect_invocation_rows_build(flow,data);
    }
    if (!xir_compile_work(flow->owner->work,7+5*sizeof(void *)+sizeof(bool))) return XR_XIR_BUDGET;
    if (data->values!=flow->values || data->parameters!=flow->function->parameter_count ||
        data->instruction_count!=flow->function->instruction_count ||
        (data->values && (!data->types || !data->write_heads)) ||
        (data->instruction_count && (!data->instruction_refs || !data->write_next)) ||
        data->instruction_storage_count>data->instruction_count ||
        !!data->instructions!=!!data->instruction_storage_count) return XR_XIR_BAD_STRUCTURE;
    flow->types=data->types;flow->instructions=data->instructions;
    flow->instruction_refs=data->instruction_refs;flow->instruction_storage_count=data->instruction_storage_count;
    flow->write_heads=data->write_heads;flow->write_next=data->write_next;flow->types_ready=true;
    return XR_XIR_OK;
}
