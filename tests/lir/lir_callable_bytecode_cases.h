/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_bytecode_cases.h - Authenticate raw function values and signatures
 */
#include "xray_native_function.h"
#include "vm/register/xrbc_code_owner.h"

typedef struct CallableRawFixture {
    const XrRbcModule *original;
    const XrRbcValueCode *original_code;
    XrRbcModule module;
    XrRbcFunction functions[3];
    const XrRbcValueCode *codes[3];
    XrRbcCallableEntry entries[3];
    XrRbcValueCode code;
    XrLirValueHomes homes;
    XrLirValueHome *values;
    XrLirHomeBytes *bytes;
    XrLirHomeEvent *events;
    XrRbcValueAccess *accesses;
    uint32_t *arguments,*words;
    XrRbcEnvironment environment;
} CallableRawFixture;
static void callable_raw_reset(CallableRawFixture *raw) {
    const XrRbcModule *original=raw->original;
    raw->module=*original;
    raw->module.code_lease=(XrRbcCodeLease){0};
    memcpy(raw->functions,original->functions,original->function_count*sizeof(*raw->functions));
    for (size_t n=0;n<original->function_count;++n) raw->codes[n]=original->borrowed_value_codes[n];
    memcpy(raw->entries,original->borrowed_callable_entries,original->function_count*sizeof(*raw->entries));
    raw->code=*raw->original_code; raw->homes=*raw->original_code->homes;
    memcpy(raw->values,raw->homes.values,((size_t)raw->code.value_count+1)*sizeof(*raw->values));
    memcpy(raw->bytes,raw->homes.bytes,raw->homes.byte_count*sizeof(*raw->bytes));
    memcpy(raw->events,raw->homes.events,raw->homes.event_count*sizeof(*raw->events));
    memcpy(raw->accesses,raw->code.accesses,raw->code.access_count*sizeof(*raw->accesses));
    if (raw->code.argument_count) memcpy(raw->arguments,raw->code.arguments,raw->code.argument_count*sizeof(*raw->arguments));
    memcpy(raw->words,raw->functions[0].code,raw->functions[0].code_count*sizeof(*raw->words));
    raw->homes.values=raw->values;
    raw->homes.bytes=raw->bytes; raw->homes.events=raw->events;
    raw->code.homes=&raw->homes; raw->code.accesses=raw->accesses; raw->code.arguments=raw->arguments;
    raw->codes[0]=&raw->code;
    raw->module.functions=raw->functions;
    raw->module.borrowed_value_codes=raw->codes;
    raw->module.borrowed_callable_entries=raw->entries;
    raw->functions[0].code=raw->words;
    if (raw->functions[0].environment_count) {
        CHECK(raw->functions[0].environment_count==1);
        raw->environment=raw->functions[0].environments[0];
        raw->functions[0].environments=&raw->environment;
    }
}
static CallableRawFixture callable_raw_fixture(const XrRbcProgram *program) {
    const XrRbcModule *module=xr_rbc_program_view(program);
    CHECK(module->function_count<=3 && module->borrowed_value_codes && module->borrowed_callable_entries);
    const XrRbcValueCode *code=module->borrowed_value_codes[0];
    CHECK(code && code->homes);
    CallableRawFixture raw={.original=module,.original_code=code};
    raw.values=xr_malloc(((size_t)code->value_count+1)*sizeof(*raw.values));
    raw.bytes=xr_malloc((code->homes->byte_count ? code->homes->byte_count : 1)*sizeof(*raw.bytes));
    raw.events=xr_malloc((code->homes->event_count ? code->homes->event_count : 1)*sizeof(*raw.events));
    raw.accesses=xr_malloc(code->access_count*sizeof(*raw.accesses));
    raw.arguments=xr_malloc((code->argument_count ? code->argument_count : 1)*sizeof(*raw.arguments));
    raw.words=xr_malloc((module->functions[0].code_count+1)*sizeof(*raw.words));
    CHECK(raw.values && raw.bytes && raw.events && raw.accesses && raw.arguments && raw.words);
    return raw;
}
static void callable_raw_free(CallableRawFixture *raw) {
    xr_free(raw->values); xr_free(raw->bytes); xr_free(raw->events);
    xr_free(raw->accesses); xr_free(raw->arguments); xr_free(raw->words);
}
static uint32_t callable_raw_constructor(const CallableRawFixture *raw) {
    for (size_t n=0;n<raw->code.access_count;++n)
        if (raw->accesses[n].operation==XR_RBC_VALUE_FUNCTION_NEW) return (uint32_t)n;
    CHECK(false); return 0;
}
static void callable_raw_positive(CallableRawFixture *raw,const XrRbcProgram *program) {
    callable_raw_reset(raw);
    CHECK(!raw->homes.owned_frame_needed && !raw->homes.frame.slot_count);
    XrRbcExecution *execution=NULL;
    XrRbcStatus status=xr_rbc_module_prepare(&raw->module,0,NULL,0,xr_rbc_program_locations(program),&execution);
    if (status!=XR_RBC_OK) fprintf(stderr,"raw Frame prepare status=%u functions=%zu environments=%u\n",
        (unsigned)status,raw->module.function_count,raw->functions[0].environment_count);
    CHECK(status==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    uint64_t bits=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits==42);
    xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
}
static void callable_raw_receiving(const XrRbcProgram *program) {
    CallableRawFixture raw=callable_raw_fixture(program);
    CHECK(raw.original->function_count==3 && raw.original->callable_type_count==1);
    callable_raw_positive(&raw,program);
    for (unsigned scenario=0;scenario<16;++scenario) {
        callable_raw_reset(&raw);
        uint32_t reference=callable_raw_constructor(&raw);
        XrRbcValueAccess *constructor=&raw.accesses[reference];
        if (scenario==0) raw.module.callable_types=NULL;
        if (scenario==1) --raw.functions[0].register_callable_count;
        if (scenario==2) raw.values[constructor->result].type=0;
        if (scenario==3) raw.functions[constructor->index].callable=0;
        if (scenario==4) raw.entries[constructor->index].signature_type=UINT32_MAX;
        if (scenario==5) {
            bool replaced=false;
            for (size_t pc=0;pc<raw.functions[0].code_count;++pc)
                if ((raw.words[pc]&255)==XR_RBC_VALUE_ACCESS && (raw.words[pc]>>16)==reference) {
                    CHECK(xr_rbc_encode(&raw.words[pc],(XrRbcOperands){.op=XR_RBC_LOADI,.a=0,.immediate=1}));
                    replaced=true;
                }
            CHECK(replaced);
        }
        if (scenario==6) raw.values[constructor->result].transport=XR_LIR_CALLABLE_TRANSPORT_OWNED;
        if (scenario==7) raw.values[constructor->result].offset=raw.homes.frame.payload_size;
        if (scenario==8) raw.bytes[raw.values[constructor->result].first_bytes].size=sizeof(uint64_t);
        if (scenario==9) raw.values[constructor->result].temporary_first_owner=0;
        if (scenario==10) raw.values[constructor->result].borrow_source=constructor->result;
        if (scenario==11) raw.events[constructor->other-1].callable_materialize=true;
        if (scenario==12) raw.events[constructor->other-1].transfer_mode=XR_LIR_OWNERSHIP_TRANSFER_BORROW;
        if (scenario==13) raw.entries[constructor->index].kind=XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION;
        if (scenario==14) raw.entries[constructor->index].physical_function=UINT32_MAX;
        if (scenario==15) constructor->operation=XR_RBC_VALUE_ASSERT_CONDITION;
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&raw.module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID);
        CHECK(!execution);
    }
    callable_raw_free(&raw);
}
/* The explicit raw Frame descriptor keeps its own scalar signature. It has no
 * compiler ValueImage to contradict when exercising the narrow call boundary. */
static void callable_narrow_argument(const XrRbcProgram *program) {
    uint32_t root_words[4],callee_word;
    CHECK(xr_rbc_encode(&root_words[0],(XrRbcOperands){.op=XR_RBC_AGGREGATE_ACCESS,.immediate=0}));
    CHECK(xr_rbc_encode(&root_words[1],(XrRbcOperands){.op=XR_RBC_LOADI,.a=2,.immediate=256}));
    CHECK(xr_rbc_encode(&root_words[2],(XrRbcOperands){.op=XR_RBC_AGGREGATE_ACCESS,.immediate=1}));
    CHECK(xr_rbc_encode(&root_words[3],(XrRbcOperands){.op=XR_RBC_RET,.a=1}));
    CHECK(xr_rbc_encode(&callee_word,(XrRbcOperands){.op=XR_RBC_AGGREGATE_ACCESS,.immediate=0}));
    XrRbcAggregateAccess accesses[]={
        {.operation=XR_RBC_FUNCTION_REFERENCE,.callee=1,.slot=1,.result={.index=0}},
        {.operation=XR_RBC_FUNCTION_CALL,.source=0,.slot=1,.window=1}};
    XrRbcAggregateAccess ret={.operation=XR_RBC_FUNCTION_RETURN,.source=0,.count=1,.slot=1};
    XrRbcRegisterKind kinds[]={XR_RBC_FUNCTION,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    uint32_t callables[]={1,0,0};
    XrRbcFunction functions[]={
        {.code=root_words,.code_count=4,.register_count=3,.register_kinds=kinds,.register_kind_count=3,
         .register_callables=callables,.register_callable_count=3,.aggregate_accesses=accesses,.aggregate_access_count=2},
        {.code=&callee_word,.code_count=1,.register_count=1,.parameter_count=1,.callable=1,
         .aggregate_accesses=&ret,.aggregate_access_count=1}};
    XrRbcCallableParameter parameter={.value={XR_RBC_CALLABLE_SCALAR,XR_POD_U8}};
    XrRbcCallableType signature={.parameters=&parameter,.parameter_count=1,.result={XR_RBC_CALLABLE_SCALAR,XR_POD_I64}};
    XrRbcModule module={.functions=functions,.function_count=2,.callable_types=&signature,.callable_type_count=1};
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_INVALID);
    xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
    CHECK(xr_rbc_encode(&root_words[1],(XrRbcOperands){.op=XR_RBC_LOADI,.a=2,.immediate=255}));
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_OK);
    outcome=xr_rbc_execution_run(execution);
    uint64_t bits=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits==255);
    xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
}
static void callable_frame_receiving(const XrRbcProgram *program) {
    CallableRawFixture raw=callable_raw_fixture(program);
    CHECK(raw.original->functions[0].environment_count==1 &&
        raw.original->functions[0].environment_size==sizeof(XrtFunction)+sizeof(uint64_t));
    uint32_t callee=raw.original->functions[0].environments[0].callee;
    bool binding=raw.original->functions[callee].captures[0].kind==XR_RBC_CALLABLE_BINDING;
    callable_raw_positive(&raw,program);
    for (unsigned scenario=0;scenario<(binding ? 14u : 11u);++scenario) {
        callable_raw_reset(&raw);
        uint32_t producer=callable_raw_constructor(&raw);
        XrRbcValueAccess *constructor=&raw.accesses[producer];
        XrRbcCallableValue capture=raw.functions[callee].captures[0];
        raw.functions[callee].captures=&capture;
        if (scenario==0) raw.functions[callee].captures=NULL;
        if (scenario==1) raw.functions[callee].capture_count=raw.functions[callee].parameter_count+1;
        if (scenario==2) capture.kind=XR_RBC_CALLABLE_REFERENCE;
        if (scenario==3) raw.functions[0].environment_size=16;
        if (scenario==4) raw.environment.offset=1;
        if (scenario==5) raw.environment.producer=0;
        if (scenario==6) constructor->window=0;
        if (scenario==7) constructor->count=0;
        if (scenario==8) raw.arguments[constructor->first]=constructor->result;
        if (scenario==9 || scenario==10) {
            uint32_t reference=0; size_t reference_pc=0;
            for (size_t pc=0;pc<raw.functions[0].code_count;++pc)
                if ((raw.words[pc]&255)==XR_RBC_VALUE_ACCESS && (raw.words[pc]>>16)==producer) {
                    reference=raw.words[pc]; reference_pc=pc;
                }
            CHECK(reference);
            size_t count=raw.functions[0].code_count;
            if (scenario==9) { raw.words[count]=raw.words[count-1]; raw.words[count-1]=reference; ++raw.functions[0].code_count; }
            else CHECK(xr_rbc_jump(&raw.words[count-1],count-1,reference_pc,count));
            raw.functions[0].source_ids=NULL; raw.functions[0].source_id_count=0;
        }
        if (scenario==11 || scenario==12) {
            bool replaced=false;
            for (size_t pc=0;pc<raw.functions[0].code_count;++pc)
                if ((raw.words[pc]&255)==XR_RBC_CELL_NEW) {
                    uint32_t destination=(raw.words[pc]>>8)&255;
                    CHECK(xr_rbc_encode(&raw.words[pc],scenario==11 ?
                        (XrRbcOperands){.op=XR_RBC_MOV,.a=destination,.b=destination} :
                        (XrRbcOperands){.op=XR_RBC_LOADI,.a=destination,.immediate=1}));
                    replaced=true;
                }
            CHECK(replaced);
        }
        if (scenario==13) capture=(XrRbcCallableValue){XR_RBC_CALLABLE_SCALAR,XR_POD_I64};
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&raw.module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID);
        CHECK(!execution);
    }
    callable_raw_free(&raw);
}
