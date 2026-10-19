/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_lifetimes.c - Real owned Generator consumers preserve waits and independent lifetimes
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Iterator lifetime %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static uintptr_t iterator_cell_owner[2];
static unsigned iterator_cell_freed[2];
static void iterator_cell_free(const void *pointer) {
    for (unsigned i=0;i<2;++i) if (iterator_cell_owner[i]==(uintptr_t)pointer) {
        iterator_cell_owner[i]=0; ++iterator_cell_freed[i];
    }
}
#define XIR_RUNTIME_BEFORE_FREE(pointer) iterator_cell_free(pointer)
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
XR_DATA const XrXirProgramSpec p20_iterator_lifetime_fixture_program;
XR_DATA const uint32_t p20_iterator_lifetime_entries[4];

typedef struct IteratorFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    unsigned backend, releases;
} IteratorFixture;
typedef struct IteratorWitness { unsigned root, producer, timers, next_waits, cache_checks, exhausted; uintptr_t cell; } IteratorWitness;

static bool iterator_native(unsigned backend, bool root) {
    return backend == 3 || (backend < 2 && root == (backend == 1));
}
static void iterator_code_drop(void *owner) {
    IteratorFixture *fixture = owner;
    CHECK(!fixture->releases++ && fixture->vm && fixture->entries);
    xr_xir_compile_program_drop(fixture->vm); fixture->vm = NULL;
    xr_compile_resources_free(fixture->entries); fixture->entries = NULL;
}
static XrXirProgram *iterator_program(IteratorFixture *fixture, unsigned backend) {
    CHECK(!effects_compile_live && !runtime_live);
    XrCompileResourceLimits caps = {67108864,8388608,128000000};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture->resources = context.resources; fixture->backend = backend;
    const XrXirProgramSpec *native = &p20_iterator_lifetime_fixture_program;
    void *packet = NULL;
    CHECK(xr_compile_resources_alloc(context.resources, native->proof.length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context.resources, native->proof.length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, native->proof.bytes, native->proof.length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet, native->proof.length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, native->proof.length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_lower(checked, &native->target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(module->declarations->slot_count == 3 && module->declarations->module_count > 1);
    fixture->count = module->function_count;
    CHECK(xr_compile_resources_calloc(context.resources, fixture->count, sizeof(*fixture->entries),
        (void **)&fixture->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    CHECK(xr_xir_compile_vm_program_take(&lowered, &fixture->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = iterator_native(backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = use_native ? native->entries[f] : fixture->vm->entries[f];
        if (use_native) ++c; else ++vm;
    }
    CHECK(backend < 2 ? vm && c : backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    for (unsigned i = 0; i < 4; ++i) CHECK(fixture->root[p20_iterator_lifetime_entries[i]]);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture,iterator_code_drop};
    XrXirProgram *program = NULL;
    uint32_t generator=UINT32_MAX;
    for (uint32_t f=0;f<fixture->count;++f) if (fixture->entries[f].flags&XR_XIR_ENTRY_GENERATOR) {generator=f;break;}
    CHECK(generator!=UINT32_MAX && (fixture->entries[generator].flags&XR_XIR_ENTRY_EXIT));
    const uint32_t rejected[]={generator,p20_iterator_lifetime_entries[0]};
    for (unsigned i=0;i<2;++i) {
        uint32_t f=rejected[i],flags=fixture->entries[f].flags;fixture->entries[f].flags^=XR_XIR_ENTRY_GENERATOR;
        CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program && !fixture->releases);
        fixture->entries[f].flags=flags;
    }
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    xr_compile_resources_release(context.resources); return program;
}

typedef struct IteratorTrace {
    IteratorFixture *fixture;
    XrXirInstance *instance;
    XrXirValue iterator, alias, held[2];
    IteratorWitness witness;
    unsigned cleanup, index;
} IteratorTrace;
static bool iterator_physical(uintptr_t address) {
    for (size_t i=0;i<runtime_owned_capacity;++i) if (runtime_owned[i].pointer) {
        uintptr_t base=(uintptr_t)runtime_owned[i].pointer;
        if (address>=base && address-base<runtime_owned[i].bytes) return true;
    }
    return false;
}
static void iterator_text(const XrXirValue *value,const char *expected) {
    const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&text,&length));
    CHECK(length==strlen(expected) && !memcmp(text,expected,length));
}
static XrXirOutputStatus iterator_output(void *context,const XrXirOutputGroup *group) {
    IteratorTrace *trace=context;
    CHECK(trace->instance && group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==701 && !trace->cleanup++);
    return XR_XIR_OUTPUT_OK;
}
/* Stopped authentic frames expose both the provider and the owned local capture. */
static void iterator_frames(IteratorTrace *trace,XrXirCall *call) {
    if (!call) return;
    CHECK(!call->driving && call->config.instance==trace->instance);
    CHECK(call->budget==&trace->instance->budget && call->config.admission.domain==trace->instance->domain);
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        IteratorFixture *fixture=trace->fixture;
        uintptr_t base=(uintptr_t)call->config.entries,pointer=(uintptr_t)frame->entry;
        CHECK(pointer>=base && (pointer-base)%sizeof(*frame->entry)==0);
        uint64_t index=(pointer-base)/sizeof(*frame->entry);CHECK(index<call->config.entry_count);
        if (index>=fixture->count) continue;
        bool root=fixture->root[index],native=iterator_native(fixture->backend,root);
        const XrXirCallEntry *entry=native ? &p20_iterator_lifetime_fixture_program.entries[index] : &fixture->vm->entries[index];
        CHECK(frame->entry->resume==entry->resume && frame->entry->environment==entry->environment);
        if (frame->entered) {
            CHECK(frame->code_kind==(native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
            if (root) trace->witness.root|=native ? 2u : 1u;
        } else CHECK(!frame->code_kind);
        if (!(frame->entry->flags&XR_XIR_ENTRY_GENERATOR)) continue;
        CHECK(!root && frame->entry->parameter_count==1 && frame->arguments);
        const XrXirValue *cell=&frame->arguments[0];CHECK(xr_xir_cell_in_domain(cell,trace->instance->domain));
        const XirCell *object=(const XirCell *)(uintptr_t)cell->payload;
        CHECK(object->initialized && !object->module_storage && !object->module_owner);
        if (trace->witness.cell) CHECK(trace->witness.cell==(uintptr_t)cell->payload);
        if (!trace->witness.cell) {
            trace->witness.cell=(uintptr_t)cell->payload;iterator_cell_owner[trace->index]=trace->witness.cell;
            XrXirValue held={0};CHECK(xr_xir_cell_read(cell,&held)==XR_XIR_VALUE_OK);
            iterator_text(&held,"cedar");xr_xir_value_drop(&held);
        }
        CHECK(iterator_physical(trace->witness.cell));
        if (frame->entered) trace->witness.producer|=native ? 2u : 1u;
    }
}
static XrXirInstanceResult iterator_tick(IteratorTrace traces[2],unsigned at) {
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(traces[1-at].instance->domain);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(traces[at].instance,1);
    iterator_frames(&traces[at],traces[at].instance->call);
    for (XirGenerator *g=traces[at].instance->executor->generators;g;g=g->active_next) {
        CHECK(g->executor==traces[at].instance->executor && g->object.domain==traces[at].instance->domain);
        iterator_frames(&traces[at],g->call);
    }
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(traces[1-at].instance->domain);
    CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    return result;
}
static void iterator_start(IteratorTrace *trace,unsigned operation,int64_t index) {
    CHECK(operation<4);XrXirValue arguments[]={trace->iterator,{XR_XIR_I64,0,index}};
    uint32_t count=operation ? operation==3 ? 2u : 1u : 0u;
    CHECK(xr_xir_instance_start(trace->instance,p20_iterator_lifetime_entries[operation],arguments,count)==XR_XIR_CALL_READY);
}
static XrXirInstanceResult iterator_wait(IteratorTrace traces[2],unsigned at,unsigned milliseconds) {
    XrXirInstanceResult result={0};unsigned polls=0;
    do {result=iterator_tick(traces,at);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED && result.epoch && result.outcome.wake);
    XrXirWaitRequest request={0};
    CHECK(xr_xir_instance_wait_request(traces[at].instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved && request.after_ms==milliseconds);
    XirGenerator *generator=(XirGenerator *)(uintptr_t)traces[at].iterator.payload;
    CHECK(generator->consumer==traces[at].instance->call && generator->call);
    uint64_t producer_polls=generator->accounting.polls;
    XrXirInstanceResult repeated=iterator_tick(traces,at);XrXirWaitRequest same={0};
    CHECK(repeated.outcome.status==XR_XIR_CALL_SUSPENDED && repeated.epoch==result.epoch && repeated.outcome.wake==result.outcome.wake);
    CHECK(xr_xir_instance_wait_request(traces[at].instance,repeated.epoch,repeated.outcome.wake,&same)==XR_XIR_CALL_READY);
    CHECK(!memcmp(&same,&request,sizeof(same)) && generator->accounting.polls==producer_polls);
    ++traces[at].witness.timers;return result;
}
static void iterator_resume(IteratorTrace *trace,XrXirInstanceResult event) {
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch+1,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
}
static XrXirValue iterator_return(IteratorTrace traces[2],unsigned at,XrXirCallStatus expected) {
    XrXirInstanceResult result={0};unsigned polls=0;
    do {result=iterator_tick(traces,at);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==expected);
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(traces[at].instance,&value)==(expected==XR_XIR_CALL_RETURNED ? expected : XR_XIR_CALL_BAD_STATE));
    return value;
}
static void iterator_cached(IteratorTrace traces[2],unsigned at) {
    IteratorTrace *trace=&traces[at];iterator_start(trace,2,0);
    XrXirInstanceResult timer=iterator_wait(traces,at,5);iterator_resume(trace,timer);
    XrXirValue more=iterator_return(traces,at,XR_XIR_CALL_RETURNED);CHECK(more.type==XR_XIR_BOOL && more.payload==1);xr_xir_value_drop(&more);
    XirGenerator *g=(XirGenerator *)(uintptr_t)trace->iterator.payload;uint64_t polls=g->accounting.polls;
    iterator_start(trace,2,0);more=iterator_return(traces,at,XR_XIR_CALL_RETURNED);
    CHECK(more.type==XR_XIR_BOOL && more.payload==1 && g->accounting.polls==polls);xr_xir_value_drop(&more);
    iterator_start(trace,1,0);trace->held[0]=iterator_return(traces,at,XR_XIR_CALL_RETURNED);iterator_text(&trace->held[0],"cedar");
    CHECK(g->accounting.polls==polls && !trace->cleanup);trace->witness.cache_checks+=2;
    iterator_start(trace,3,0);timer=iterator_wait(traces,at,7);iterator_resume(trace,timer);
    trace->held[1]=iterator_return(traces,at,XR_XIR_CALL_RETURNED);iterator_text(&trace->held[1],"birch");CHECK(!trace->cleanup);
    iterator_start(trace,2,0);more=iterator_return(traces,at,XR_XIR_CALL_RETURNED);
    CHECK(more.type==XR_XIR_BOOL && !more.payload && trace->cleanup==1 && !g->call && !g->consumer);
    CHECK(g->outcome.status==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&more);
}
static void iterator_nth(IteratorTrace traces[2]) {
    XrXirInstanceResult pending[2];XrXirTaskWaitToken tokens[2]={{0}};
    for (unsigned i=0;i<2;++i) {
        iterator_start(&traces[i],3,1);pending[i]=iterator_wait(traces,i,5);
        XirGenerator *g=(XirGenerator *)(uintptr_t)traces[i].iterator.payload;
        CHECK(xr_xir_call_iterator_wait_token(g->consumer,&tokens[i]));
        CHECK(tokens[i].request.kind==XR_XIR_WAIT_ITERATOR_NTH && tokens[i].request.after_ms==1 && tokens[i].request.subject==g);
    }
    CHECK(tokens[0].binding.owner!=tokens[1].binding.owner && tokens[0].request.subject!=tokens[1].request.subject);
    CHECK(xr_xir_instance_cancel_current(traces[0].instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_resume(traces[0].instance,pending[0].epoch,pending[0].outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_complete_iterator_wait(traces[0].instance->call,&tokens[0])==XR_XIR_CALL_BAD_STATE);
    XrXirValue empty=iterator_return(traces,0,XR_XIR_CALL_CANCELLED);CHECK(!empty.type && !empty.payload);
    XirGenerator *cancelled=(XirGenerator *)(uintptr_t)traces[0].iterator.payload;
    CHECK(cancelled->outcome.status==XR_XIR_CALL_CANCELLED && traces[0].cleanup==1 && !cancelled->call && !cancelled->consumer);
    iterator_resume(&traces[1],pending[1]);pending[1]=iterator_wait(traces,1,7);
    CHECK(xr_xir_call_complete_iterator_wait(traces[1].instance->call,&tokens[1])==XR_XIR_CALL_BAD_STATE);
    iterator_resume(&traces[1],pending[1]);traces[1].held[1]=iterator_return(traces,1,XR_XIR_CALL_RETURNED);
    iterator_text(&traces[1].held[1],"birch");CHECK(!traces[1].cleanup);
}

static void iterator_exhausted(IteratorTrace traces[2],unsigned at) {
    IteratorTrace *trace=&traces[at];XirGenerator *g=(XirGenerator *)(uintptr_t)trace->iterator.payload;
    uint64_t producer_polls=g->accounting.polls;
    for (unsigned repeat=0;repeat<2;++repeat) {
        iterator_start(trace,1,0);XrXirInstanceResult result={0};unsigned polls=0;
        do {result=iterator_tick(traces,at);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && result.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);
        CHECK(xr_xir_call_result_valid(&result.outcome) && !result.outcome.wake && !g->call && !g->consumer);
        XrXirValue empty={0};CHECK(xr_xir_instance_take_result(trace->instance,&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
        CHECK(trace->cleanup==1 && g->accounting.polls==producer_polls);++trace->witness.exhausted;
    }
}
static void iterator_next_timers(IteratorTrace traces[2],unsigned at) {
    IteratorTrace *trace=&traces[at];
    for (unsigned n=0;n<2;++n) {
        iterator_start(trace,1,0);XrXirInstanceResult timer=iterator_wait(traces,at,n ? 7 : 5);
        ++trace->witness.next_waits;iterator_resume(trace,timer);
        trace->held[n]=iterator_return(traces,at,XR_XIR_CALL_RETURNED);iterator_text(&trace->held[n],n ? "birch" : "cedar");
        CHECK(!trace->cleanup);
    }
    iterator_start(trace,2,0);XrXirValue more=iterator_return(traces,at,XR_XIR_CALL_RETURNED);
    CHECK(more.type==XR_XIR_BOOL && !more.payload && trace->cleanup==1);xr_xir_value_drop(&more);
}

static void iterator_graph(unsigned backend,unsigned order,unsigned mode) {
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    IteratorFixture fixture={0};XrXirProgram *program=iterator_program(&fixture,backend);
    IteratorTrace traces[2]={{0}};XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for (unsigned i=0;i<2;++i) {
        traces[i].fixture=&fixture;traces[i].index=i;config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterator_output,&traces[i]};
        CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
    }
    CHECK(traces[0].instance->domain!=traces[1].instance->domain);xr_xir_compile_program_drop(program);CHECK(!fixture.releases);
    uintptr_t addresses[2]={0};
    for (unsigned i=0;i<2;++i) {
        iterator_start(&traces[i],0,0);traces[i].iterator=iterator_return(traces,i,XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_valid(&traces[i].iterator) && xr_xir_value_copy(&traces[i].iterator,&traces[i].alias)==XR_XIR_VALUE_OK);
        XirGenerator *g=(XirGenerator *)(uintptr_t)traces[i].iterator.payload;
        CHECK(g->object.kind==XR_XIR_TYPE_ITERATOR && g->object.domain==traces[i].instance->domain && g->executor==traces[i].instance->executor);
        addresses[i]=(uintptr_t)g;CHECK(iterator_physical(addresses[i]));
    }
    for (unsigned i=0;i<2;++i) {
        XrXirValue foreign[]={traces[1-i].iterator,{XR_XIR_I64,0,1}};uint64_t epoch=traces[i].instance->epoch;
        size_t attempts=runtime_attempts;CHECK(xr_xir_instance_start(traces[i].instance,p20_iterator_lifetime_entries[3],foreign,2)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(traces[i].instance->epoch==epoch && runtime_attempts==attempts && !traces[i].cleanup);
    }
    if (mode==1) iterator_nth(traces);
    else for (unsigned i=0;i<2;++i) {
        if (mode==2) iterator_next_timers(traces,i);else iterator_cached(traces,i);
        iterator_exhausted(traces,i);
    }
    for (unsigned i=0;i<2;++i) {
        CHECK(traces[i].witness.root==(iterator_native(backend,true) ? 2u : 1u));
        CHECK(traces[i].witness.producer==(iterator_native(backend,false) ? 2u : 1u) && traces[i].witness.cell);
        CHECK(traces[i].witness.timers==(mode==1 && !i ? 1u : 2u));CHECK(iterator_cell_freed[i]<=1);
    }
    for (unsigned i=0;i<2;++i) {
        unsigned at=i ? 1-order : order;CHECK(xr_xir_instance_free(traces[at].instance)==XR_XIR_CALL_READY);traces[at].instance=NULL;
        CHECK(traces[at].cleanup==1);
    }
    CHECK(fixture.releases==1 && !fixture.vm && !fixture.entries);
    size_t drop_attempts=runtime_attempts;
    for (unsigned i=0;i<2;++i) {
        const XirGenerator *g=(const XirGenerator *)(uintptr_t)traces[i].iterator.payload;
        CHECK(xr_xir_value_valid(&traces[i].iterator) && !g->executor && !g->call && !g->consumer);
        CHECK(g->outcome.status==(mode==1 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
        xr_xir_value_drop(&traces[i].iterator);CHECK(iterator_physical(addresses[i]));xr_xir_value_drop(&traces[i].alias);
        CHECK(!iterator_physical(addresses[i]) && iterator_cell_freed[i]==1 && !iterator_cell_owner[i]);
    }
    CHECK(runtime_attempts==drop_attempts && !effects_compile_live && !effects_compile_bytes);
    for (unsigned i=0;i<2;++i) for (unsigned j=0;j<2;++j) if (traces[i].held[j].type) {
        iterator_text(&traces[i].held[j],j ? "birch" : "cedar");xr_xir_value_drop(&traces[i].held[j]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("P20 Iterator backend=%u order=%u mode=%u realOwnedCell timers=%u nextWaits=%u cacheChecks=%u exhausted=%u staleWaitReject peerFees codeLease1 lateStrings physical0/0\n",backend,order,mode,traces[0].witness.timers+traces[1].witness.timers,traces[0].witness.next_waits+traces[1].witness.next_waits,traces[0].witness.cache_checks+traces[1].witness.cache_checks,traces[0].witness.exhausted+traces[1].witness.exhausted);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    for(unsigned order=0;order<2;++order)for(unsigned mode=0;mode<3;++mode)iterator_graph(backend,order,mode);
    return 0;
}
