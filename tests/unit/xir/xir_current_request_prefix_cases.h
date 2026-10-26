/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_current_request_prefix_cases.h - Current reader argument prefix regressions
 *
 * KEY CONCEPT:
 *   Real Input remains live and unchanged while Request copies lose required
 *   fields. Both projections must reject before polling or publishing output.
 */
#ifndef XIR_CURRENT_REQUEST_PREFIX_CASES_H
#define XIR_CURRENT_REQUEST_PREFIX_CASES_H

typedef struct PrefixOutput {
    bool active,copy;
    XrXirType type;
    uint32_t source;
    XrXirInstruction terminal;
} PrefixOutput;
static PrefixOutput prefix_sentinel(void) {
    PrefixOutput result={.active=false,.copy=true,.type=XR_XIR_UNIT,.source=42};
    memset(&result.terminal,0xA5,sizeof(result.terminal));return result;
}
static bool prefix_unchanged(const PrefixOutput *result) {
    PrefixOutput initial=prefix_sentinel();
    return result->active==initial.active && result->copy==initial.copy &&
        result->type==initial.type && result->source==initial.source &&
        !memcmp(&result->terminal,&initial.terminal,sizeof(result->terminal));
}
static void prefix_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
/* Physical reader rows deliberately isolate classification from public
 * instruction admission. Every valid opcode still has a complete live frame. */
static void prefix_fixture(EdgeFixture *f,unsigned kind) {
    static const XrXirOp ops[]={XR_XIR_ENUM_NEW,XR_XIR_CONST_INT,XR_XIR_COPY,XR_XIR_COPY,
        XR_XIR_GO,XR_XIR_GENERATOR_NEW,XR_XIR_INVOKE_ERROR};
    static const XrXirType types[]={(XrXirType)EE_ENUM,XR_XIR_I64,(XrXirType)EE_ENUM,
        XR_XIR_BOOL,XR_XIR_UNIT,XR_XIR_UNIT,XR_XIR_UNIT};
    static const XrXirType parameters[]={(XrXirType)EE_ENUM,XR_XIR_BOOL,(XrXirType)EE_CELL};
    CHECK(kind<7);ee_fixture(f,EE_JUMP,false);
    f->functions[1].parameters=parameters;f->functions[1].parameter_count=3;
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    f->subject[1]=(XrXirInstruction){.op=ops[kind],.type=types[kind],.args={kind==3?1u:0u,0}};
    f->subject[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->blocks[0]=(XrXirBlock){.count=3};
    f->functions[1].instruction_count=3;f->functions[1].block_count=1;
    if(kind==4 || kind==5) f->subject[1].immediate=2;
    if(kind==5) f->identities[2].promises|=XR_XIR_FUNCTION_GENERATOR;
    if(kind==6) {
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_INVOKE,.targets={2,1},.immediate=2};
        f->subject[1].immediate=0;
        f->subject[2]=(XrXirInstruction){.op=XR_XIR_THROW,.args={4}};
        f->subject[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
        f->blocks[0]=(XrXirBlock){.count=1};
        f->blocks[1]=(XrXirBlock){.first=1,.count=2};
        f->blocks[2]=(XrXirBlock){.first=3,.count=1};
        f->functions[1].instruction_count=4;f->functions[1].block_count=3;
    }
}
static void prefix_open(XrXirCompileContext *work,EdgeFixture *fixture,
    EffectBodyMaterial **material,EffectBodyCurrentRead *current,EffectErrorEquationRead *read) {
    CHECK(effect_body_material_create(work,&fixture->module,material)==XR_XIR_OK);
    CHECK(effect_body_current_open(work,*material,current)==XR_XIR_OK);
    *read=(EffectErrorEquationRead){.body=effect_body_owned_request(work,*material),.work=work,.function=1};
    read->body.current=current;
    CHECK(effect_body_request_head(&read->body,1,&read->head)==XR_XIR_OK);
    CHECK(read->head.parameter_count==3 && read->head.parameters[2]==(XrXirType)EE_CELL && (*material)->cells);
}
static void prefix_close(XrXirCompileContext *work,EffectBodyMaterial *material,
    EffectBodyCurrentRead *current,uint64_t baseline) {
    *current=(EffectBodyCurrentRead){0};effect_body_material_free(material);pc_release(work,baseline);
}
static uint32_t prefix_delegated_calls;
static XrXirStatus prefix_delegated(const EffectBodyRequest *request,uint32_t function,
    uint32_t instruction,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    ++prefix_delegated_calls;
    return effect_body_owned_instruction(request,function,instruction,storage,output,fields);
}
static void prefix_adapter(EffectErrorEquationRead *read,const XrXirCompileContext *work,
    const EdgeFixture *fixture,unsigned adapter) {
    CHECK(adapter<4);
    if(adapter==1) read->body=effect_body_raw_request(work,&fixture->module);
    else if(adapter==2) read->body.current=NULL;
    else if(adapter==3) read->body.instruction=prefix_delegated;
}
static XrXirStatus prefix_call(unsigned helper,const EffectErrorEquationRead *read,
    XrXirCompileContext *work,uint32_t value,PrefixOutput *output) {
    if(helper) return error_equation_edge_identity(read,1,1,&output->terminal);
    ErrorFlow flow={.body=read->body,.function=&read->head,.function_index=read->function,.remaining=work};
    return error_row_active(&flow,value,&output->active,&output->type,&output->copy,&output->source);
}
static void prefix_literal(unsigned kind,const PrefixOutput *result) {
    static const bool active[]={true,false,true,false,true,true,true};
    static const XrXirType type[]={(XrXirType)EE_ENUM,XR_XIR_I64,(XrXirType)EE_ENUM,
        XR_XIR_BOOL,XR_XIR_UNIT,XR_XIR_UNIT,XR_XIR_UNIT};
    CHECK(result->active==active[kind] && result->type==type[kind]);
    CHECK(result->copy==(kind==2 || kind==3));
    CHECK(result->source==(kind==2?0u:kind==3?1u:UINT32_MAX));
}
static void prefix_valid(void) {
    for(unsigned kind=0;kind<7;++kind) {
        EdgeFixture f;prefix_fixture(&f,kind);
        XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
        EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead original;
        prefix_open(&work,&f,&material,&current,&original);
        for(unsigned adapter=0;adapter<4;++adapter) for(unsigned helper=0;helper<2;++helper) {
            EffectErrorEquationRead read=original;prefix_adapter(&read,&work,&f,adapter);
            PrefixOutput result=prefix_sentinel();size_t allocated=attempts;uint64_t physical=live_bytes;
            uint32_t calls=prefix_delegated_calls;
            pc_allocate_hook=prefix_no_callback;pc_free_hook=prefix_no_callback;
            CHECK(prefix_call(helper,&read,&work,read.head.parameter_count+1,&result)==XR_XIR_OK);
            if(!helper) prefix_literal(kind,&result);
            else CHECK(result.terminal.op==f.subject[1].op && result.terminal.args[0]==f.subject[1].args[0] &&
                result.terminal.targets[1]==f.subject[1].targets[1]);
            CHECK(prefix_delegated_calls==calls+(adapter==3?1u:0u));
            if(!helper) {
                result=prefix_sentinel();CHECK(prefix_call(0,&read,&work,1,&result)==XR_XIR_OK);
                CHECK(!result.active && result.type==XR_XIR_BOOL && !result.copy && result.source==UINT32_MAX);
            }
            CHECK(attempts==allocated && live_bytes==physical);
            pc_allocate_hook=NULL;pc_free_hook=NULL;
        }
        prefix_close(&work,material,&current,baseline);
    }
}
typedef struct PrefixPoll { uint32_t calls;bool requested; } PrefixPoll;
static bool prefix_poll(const void *owner) {
    PrefixPoll *poll=(PrefixPoll *)owner;++poll->calls;return poll->requested;
}
static uint64_t prefix_setup_work(void) {
    EdgeFixture f;prefix_fixture(&f,0);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    prefix_open(&work,&f,&material,&current,&read);uint64_t setup=pc_stats(&work).work;
    prefix_close(&work,material,&current,baseline);return setup;
}
/* Only the Request copy changes. Invalid cases collect exact status and
 * output before releasing their independent owners, even on the old red run. */
static bool prefix_bad(unsigned helper,unsigned kind,unsigned budget,unsigned adapter,uint64_t setup) {
    EdgeFixture f;prefix_fixture(&f,0);XrCompileResourceLimits caps=pc_caps();
    if(budget==1 || budget==2) caps.work=setup+(budget==1?0u:7u);
    XrXirCompileContext work=pc_owner(caps);uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    prefix_open(&work,&f,&material,&current,&read);CHECK(pc_stats(&work).work==setup);
    prefix_adapter(&read,&work,&f,adapter);
    if(!kind) read.body.owner=NULL;
    else if(kind==1) read.body.domain.types=NULL;
    else if(kind==2) read.body.domain.function_count=0;
    else CHECK(helper && kind==3);
    PrefixPoll poll={0,budget==3};
    if(budget==3) CHECK(xr_compile_resources_begin_cancel(work.resources,prefix_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    PrefixOutput result=prefix_sentinel();size_t allocated=attempts;
    uint64_t before=pc_stats(&work).work,physical=live_bytes;
    pc_allocate_hook=prefix_no_callback;pc_free_hook=prefix_no_callback;
    XrXirStatus actual=kind==3?error_equation_edge_identity(&read,1,1,NULL):
        prefix_call(helper,&read,&work,read.head.parameter_count+1,&result);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    bool correct=actual==XR_XIR_BAD_STRUCTURE && prefix_unchanged(&result) &&
        pc_stats(&work).work==before && !poll.calls && attempts==allocated && live_bytes==physical;
    if(!correct) fprintf(stderr,"request-prefix helper%u kind%u budget%u adapter%u actual%u expected%u work%llu poll%u unchanged%u\n",
        helper,kind,budget,adapter,(unsigned)actual,(unsigned)XR_XIR_BAD_STRUCTURE,
        (unsigned long long)(pc_stats(&work).work-before),poll.calls,(unsigned)prefix_unchanged(&result));
    if(budget==3) {
        CHECK(xr_compile_resources_end_cancel(work.resources,prefix_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
        CHECK(poll.calls==1);
    }
    prefix_close(&work,material,&current,baseline);return correct;
}
static void prefix_fee_sample(unsigned helper,uint64_t *setup,uint64_t *fee) {
    EdgeFixture f;prefix_fixture(&f,0);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    prefix_open(&work,&f,&material,&current,&read);*setup=pc_stats(&work).work;
    PrefixOutput result=prefix_sentinel();size_t allocated=attempts;uint64_t physical=live_bytes;
    CHECK(prefix_call(helper,&read,&work,read.head.parameter_count+1,&result)==XR_XIR_OK);
    *fee=pc_stats(&work).work-*setup;CHECK(*fee && attempts==allocated && live_bytes==physical);
    if(!helper) prefix_literal(0,&result);
    else CHECK(result.terminal.op==f.subject[1].op && result.terminal.args[0]==0 && result.terminal.targets[1]==f.subject[1].targets[1]);
    prefix_close(&work,material,&current,baseline);
}
static void prefix_fee_boundary(unsigned helper,uint64_t setup,uint64_t fee,bool less) {
    EdgeFixture f;prefix_fixture(&f,0);XrCompileResourceLimits caps=pc_caps();
    CHECK(fee && setup<=UINT64_MAX-fee);caps.work=setup+fee-(less?1u:0u);
    XrXirCompileContext work=pc_owner(caps);uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    prefix_open(&work,&f,&material,&current,&read);CHECK(pc_stats(&work).work==setup);
    PrefixOutput result=prefix_sentinel();
    XrXirStatus actual=prefix_call(helper,&read,&work,read.head.parameter_count+1,&result);
    if(less) {
        CHECK(actual==XR_XIR_BUDGET);
        /* A valid terminal read may publish earlier scratch columns on a
         * later quota failure. Only its selected target remains untouched. */
        if(helper) CHECK(result.terminal.targets[1]==UINT32_C(0xA5A5A5A5));
        else CHECK(prefix_unchanged(&result));
    } else {
        CHECK(actual==XR_XIR_OK && pc_stats(&work).work==caps.work);
        if(!helper) prefix_literal(0,&result);
        else CHECK(result.terminal.targets[1]==f.subject[1].targets[1]);
    }
    prefix_close(&work,material,&current,baseline);
}
static void prefix_foreign(void) {
    EdgeFixture f;prefix_fixture(&f,0);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead original;
    prefix_open(&work,&f,&material,&current,&original);XrXirCompileContext copy=work;
    for(unsigned helper=0;helper<2;++helper) {
        EffectErrorEquationRead read=original;read.body.read.owner=&copy;PrefixOutput result=prefix_sentinel();
        CHECK(prefix_call(helper,&read,&work,read.head.parameter_count+1,&result)==XR_XIR_BAD_STRUCTURE);
        CHECK(prefix_unchanged(&result));
    }
    prefix_close(&work,material,&current,baseline);
}
static void prefix_frame(void) {
    EdgeFixture f;prefix_fixture(&f,0);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    prefix_open(&work,&f,&material,&current,&read);
    current=(EffectBodyCurrentRead){0};read.body.current=NULL;
    EffectBodyRun *run=&material->functions[1].runs[0];XrXirOp opcode=run->base.op;
    run->base.op=XR_XIR_INVALID;
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);
    CHECK(!current.work && !current.material);run->base.op=opcode;
    uint32_t mask=run->mask,delta=0;
    if(mask&(UINT32_C(1)<<7)) delta=run->delta[7];
    run->mask|=UINT32_C(1)<<7;run->delta[7]=0;
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);
    CHECK(!current.work && !current.material);run->mask=mask;
    if(mask&(UINT32_C(1)<<7)) run->delta[7]=delta;
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_OK);read.body.current=&current;
    for(unsigned helper=0;helper<2;++helper) {
        PrefixOutput result=prefix_sentinel();
        CHECK(prefix_call(helper,&read,&work,read.head.parameter_count+1,&result)==XR_XIR_OK);
    }
    prefix_close(&work,material,&current,baseline);
}
static void finite_request_prefix_cases(void) {
    CHECK(!live && !live_bytes && !pc_allocate_hook && !pc_free_hook);
    uint64_t setup=prefix_setup_work();unsigned failures=0;
    /* The original red invocation records both missing domain rejects, then
     * frees every fixture before failing the same unchanged correctness gate. */
    for(unsigned helper=0;helper<2;++helper) for(unsigned kind=0;kind<3;++kind) {
        for(unsigned budget=0;budget<4;++budget)
            if(!prefix_bad(helper,kind,budget,0,setup)) ++failures;
        for(unsigned adapter=1;adapter<4;++adapter)
            if(!prefix_bad(helper,kind,0,adapter,setup)) ++failures;
    }
    /* An already failing original domain prefix must not enter its formerly
     * unchecked NULL storage. Every successful qualification covers it. */
    if(!failures) {
        for(unsigned budget=0;budget<4;++budget)
            if(!prefix_bad(1,3,budget,0,setup)) ++failures;
        for(unsigned adapter=1;adapter<4;++adapter)
            if(!prefix_bad(1,3,0,adapter,setup)) ++failures;
    }
    prefix_valid();prefix_foreign();prefix_frame();
    for(unsigned helper=0;helper<2;++helper) {
        uint64_t measured=0,fee=0;prefix_fee_sample(helper,&measured,&fee);
        prefix_fee_boundary(helper,measured,fee,false);prefix_fee_boundary(helper,measured,fee,true);
    }
    CHECK(!live && !live_bytes && !pc_allocate_hook && !pc_free_hook);
    CHECK(failures==0);
    puts("request prefix: direct row and terminal, exact rejection, adapters, fees and physical0 PASS");
}
#endif // XIR_CURRENT_REQUEST_PREFIX_CASES_H
