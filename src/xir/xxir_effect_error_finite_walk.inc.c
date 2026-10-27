/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_finite_walk.inc.c - Current sparse Error equation domain
 *
 * KEY CONCEPT:
 *   Current opcode and type fields determine zero equations. No stored
 *   membership claim or previous successful read can omit an equation.
 */

typedef XrXirStatus (*ErrorFiniteVisit)(EffectErrorFiniteRead *,const EffectErrorEquationRead *,
    uint32_t,uint64_t,void *,bool *);

/* An actual complete opcode walk proves zero escaping without interpreting
 * irrelevant local states. Every unknown operation retains the full domain. */
static XrXirStatus error_finite_empty(const EffectErrorFiniteRead *read,
    uint32_t function,bool *empty) {
    uint32_t actual=UINT32_MAX;
    XrXirStatus status=error_finite_context_read(read,function,&actual);
    if(status!=XR_XIR_OK) return status;
    /* No escaping errors does not prove the returned carrier has no errors. */
    if(read->proof->rows[function].returned) { *empty=false;return XR_XIR_OK; }
    EffectBodyRequest request=effect_body_owned_request(read->work,read->proof->material);
    if (read->current) request.current=&read->current->input;
    if (error_native(&request,actual)) {
        EffectBodyFunctionHead head={0};status=effect_body_request_head(&request,actual,&head);
        if (status==XR_XIR_OK) status=error_native_header(&request,actual,&head);
        *empty=false;return status;
    }
    const EffectBodyFunction *head=&read->proof->material->functions[actual];
    EffectBodyRead body=(EffectBodyRead){.owner=read->work};
    if(!read->current) status=effect_role_range(read->work,head->runs,head->run_capacity,sizeof(*head->runs));
    if(status!=XR_XIR_OK) return status;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};*empty=false;
    for(uint32_t run=0;run<head->run_count;++run) {
        const EffectBodyRun *ref=NULL;status=effect_body_range_ref(&body,head,&cursor,&ref);
        if(status!=XR_XIR_OK) return status;
        bool constant=!(ref->mask&UINT32_C(1));
        uint32_t count=constant?1u:ref->count;
        for(uint32_t i=0;i<count;++i) {
            uint32_t opcode=XR_XIR_INVALID;
            status=effect_body_run_word(&body,ref,ref->first+i,0,&opcode);
            if(status!=XR_XIR_OK) return status;
            if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            if(!effect_physical_error_empty_opcode((XrXirOp)opcode)) return XR_XIR_OK;
        }
    }
    if(cursor.first!=head->instruction_count || cursor.run!=head->run_count)
        return XR_XIR_BAD_STRUCTURE;
    *empty=true;return XR_XIR_OK;
}
static XrXirStatus error_finite_carrier(EffectErrorFiniteRead *read,const EffectErrorEquationRead *equation,
    uint32_t function,uint32_t value,ErrorFiniteVisit visit,void *owner,bool *paused) {

    uint32_t blocks=read->proof->rows[function].blocks;
    for(uint32_t b=0;b<blocks;++b) {

        for(uint32_t w=0;w<read->proof->words;++w) {

            XrXirStatus status=visit(read,equation,function,error_finite_entry_coordinate(read->proof,function,b,value,w),owner,paused);
            if(status!=XR_XIR_OK || *paused) return status;
        }
    }
    return XR_XIR_OK;
}
/* Only scalar zero rows are omitted. GO and INVOKE_ERROR remain active even
 * when their physical result type alone would classify as a closed scalar. */
static XrXirStatus error_finite_fields_active(ErrorFlow *view,const EffectBodyRead *body,
    const EffectBodyRun *ref,uint32_t instruction,bool *active) {
    uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT;
    XrXirStatus status=effect_body_run_word(body,ref,instruction,0,&opcode);
    if(status==XR_XIR_OK) status=effect_body_run_word(body,ref,instruction,1,&type);
    if(status!=XR_XIR_OK) return status;
    if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if(opcode==XR_XIR_GO || opcode==XR_XIR_INVOKE_ERROR) { *active=true;return XR_XIR_OK; }
    return error_type_active(view,(XrXirType)type,active);
}
/* Membership depends on current opcode and type, not on a block coordinate.
 * Within this Input-stable walk, classify each value once before visiting all
 * of its blocks. A full fact buffer pauses before allocation or release
 * callbacks; no membership result survives growth or a fresh Input view. */
static XrXirStatus error_finite_values(EffectErrorFiniteRead *read,const EffectErrorEquationRead *equation,
    uint32_t function,ErrorFlow *view,ErrorFiniteVisit visit,void *owner,bool *paused) {
    const EffectBodyFunction *head=&read->proof->material->functions[view->function_index];
    XrXirStatus status=XR_XIR_OK;
    for(uint32_t p=0;p<head->parameter_count;++p) {

        bool active=false;status=error_type_active(view,head->parameters[p],&active);
        if(status!=XR_XIR_OK) return status;
        if(active) { status=error_finite_carrier(read,equation,function,p,visit,owner,paused);if(status!=XR_XIR_OK || *paused) return status; }
    }
    EffectBodyRead body=(EffectBodyRead){.owner=read->work};
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for(uint32_t run=0;run<head->run_count;++run) {
        const EffectBodyRun *ref=NULL;status=effect_body_range_ref(&body,head,&cursor,&ref);
        if(status!=XR_XIR_OK) return status;
        bool constant=!(ref->mask&UINT32_C(3)),active=false;
        if(constant) {
            status=error_finite_fields_active(view,&body,ref,ref->first,&active);
            if(status!=XR_XIR_OK) return status;
            if(!active) continue;
        }
        for(uint32_t i=0;i<ref->count;++i) {
            if(!constant) {
                status=error_finite_fields_active(view,&body,ref,ref->first+i,&active);
                if(status!=XR_XIR_OK) return status;
            }
            if(!active) continue;
            status=error_finite_carrier(read,equation,function,head->parameter_count+ref->first+i,visit,owner,paused);
            if(status!=XR_XIR_OK || *paused) return status;
        }
    }
    if(cursor.first!=head->instruction_count || cursor.run!=head->run_count)
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}
static XrXirStatus error_finite_walk(const EffectErrorFiniteRead *read,
    uint32_t function,ErrorFiniteVisit visit,void *owner,bool *empty) {
    bool paused=false;
    XrXirStatus status=error_finite_empty(read,function,empty);
    if(status!=XR_XIR_OK || *empty) return status;

    EffectErrorFiniteRead current=*read;
    EffectErrorEquationRead equation;status=error_finite_equation(&current,function,&equation);
    if(status!=XR_XIR_OK) return status;
    const EffectErrorFiniteRow *row=&read->proof->rows[function];
    XrXirCompileContext budget=*read->work;
    ErrorFlow view={.body=equation.body,.function=&equation.head,.function_index=equation.function,.remaining=&budget};
    for(uint32_t b=0;b<row->blocks;++b) {
        status=visit(&current,&equation,function,row->first+b,owner,&paused);if(status!=XR_XIR_OK || paused) return status;
    }
    if(row->blocks) {
        status=error_finite_values(&current,&equation,function,&view,visit,owner,&paused);
        if(status!=XR_XIR_OK || paused) return status;
    }
    for(uint32_t w=0;w<read->proof->words;++w) {
        status=visit(&current,&equation,function,row->end-(uint64_t)read->proof->words*64+(uint64_t)w*64,owner,&paused);
        if(status!=XR_XIR_OK || paused) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_compare(EffectErrorFiniteRead *read,const EffectErrorEquationRead *equation,
    uint32_t function,uint64_t coordinate,void *owner,bool *paused) {
    (void)owner;*paused=false;uint64_t expected=0,actual=0;uint32_t bits=0;
    const EffectErrorFiniteRow *row=&read->proof->rows[function];

    bool reach=coordinate>=row->first && coordinate-row->first<row->blocks;
    XrXirStatus status=error_finite_word(read,coordinate,reach?1:64,&actual);
    if(status!=XR_XIR_OK) return status;
    /* A positive Boolean row is already top. The independent strict-rank
     * support pass still proves its equality and groundedness.
     * Zero reachability rows and every carrier word need full closure. */
    if(reach && actual) return XR_XIR_OK;
    status=error_finite_rhs_walk(read,equation,function,coordinate,&expected,&bits);
    if(status!=XR_XIR_OK) return status;
    return actual==expected?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
