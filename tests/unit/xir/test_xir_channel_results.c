/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_results.c - Owned receive publication and real failure preservation
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "xir/xxir_channel.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_array.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_nominal.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_channel_value_observer.h"

enum { RESULT_ARRAY=256, RESULT_CHANNEL_I64, RESULT_CHANNEL_STRING, RESULT_CHANNEL_ARRAY,
    RESULT_CHANNEL_NESTED, RESULT_I64, RESULT_STRING, RESULT_ARRAY_I64, RESULT_NESTED, RESULT_SEND,
    RESULT_ARRAY_CHANNEL, RESULT_DEEP_ARRAY, RESULT_CHANNEL_DEEP, RESULT_DEEP,
    RESULT_CHANNEL_UNIT, RESULT_UNIT, RESULT_UNIT_ARRAY, RESULT_CHANNEL_UNIT_RESULTS, RESULT_UNIT_RESULTS };
typedef struct ResultOwner {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValueAdmission admission;
} ResultOwner;
static bool result_empty(const XrXirValue *value) { return !value->type && !value->reserved && !value->payload; }
static XrXirValue result_integer(int64_t number) { return (XrXirValue){XR_XIR_I64,0,number}; }
static XrXirValueStatus result_compile_status(XrXirStatus status) {
    return status==XR_XIR_OK ? XR_XIR_VALUE_OK : status==XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM :
        status==XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus result_owner_new(ResultOwner *owner) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};
    owner->context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus resource=xr_compile_resources_new(&limits,&owner->context.resources);
    if (resource!=XR_COMPILE_RESOURCE_OK) return resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_RECV);
    const XrNativeTypeDeclaration *send=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_SEND_RESULT);
    const char *canonical="stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def";
    XrXirConstraint unconstrained={0};
    XrXirNominalField field={{"value",5},(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,0};
    XrXirNominalVariant recv_variants[]={{{"Value",5},0,1},{{"Empty",5},1,0},{{"Timeout",7},1,0},{{"Closed",6},1,0}};
    XrXirNominalVariant send_variants[]={{{"Sent",4},0,0},{{"Full",4},0,0},{{"Timeout",7},0,0},{{"Closed",6},0,0}};
    XrXirNominalDeclaration declarations[2]={
        {.module={canonical,(uint32_t)strlen(canonical)},.name={"Recv",4},.exported=1,
         .constraints=&unconstrained,.parameter_count=1,.fields=&field,.field_count=1,
         .kind=XR_XIR_NOMINAL_ENUM,.variants=recv_variants,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_RECV}},
        {.module={canonical,(uint32_t)strlen(canonical)},.name={"SendResult",10},.exported=1,
         .kind=XR_XIR_NOMINAL_ENUM,.variants=send_variants,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_SEND_RESULT}}};
    memcpy(declarations[0].native.source_fingerprint,native->source_fingerprint.bytes,32);
    memcpy(declarations[1].native.source_fingerprint,send->source_fingerprint.bytes,32);
    XrXirNominalTable table={declarations,2,NULL};
    XrXirType elements[]={XR_XIR_I64,XR_XIR_STRING,(XrXirType)RESULT_ARRAY,(XrXirType)RESULT_CHANNEL_STRING,(XrXirType)RESULT_DEEP_ARRAY,XR_XIR_UNIT,(XrXirType)RESULT_UNIT_ARRAY};
    XrXirTypeNode nodes[19]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)RESULT_ARRAY},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)RESULT_CHANNEL_STRING},
        {.kind=XR_XIR_TYPE_NOMINAL}, {.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_NOMINAL}, {.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)RESULT_CHANNEL_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)RESULT_ARRAY_CHANNEL},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)RESULT_DEEP_ARRAY},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&elements[4],1,&elements[4],1}},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&elements[5],1,&elements[5],1}},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)RESULT_UNIT},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)RESULT_UNIT_ARRAY},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&elements[6],1,&elements[6],1}}};
    for (unsigned i=0;i<4;++i) nodes[5+i].nominal=(XrXirNominalType){0,&elements[i],1,&elements[i],1};
    XrXirTypes types={nodes,19,&table,NULL};
    XrXirStatus compiled=xr_xir_compile_types_structure_verify(&owner->context,&types);
    XrXirFunction function={0}; XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
    XrXirConstraint sendable={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    for (uint32_t type=RESULT_CHANNEL_I64;compiled==XR_XIR_OK && type<=RESULT_UNIT_RESULTS;++type)
    {
        compiled=xr_xir_compile_type_constraints(&owner->context,&module,0,(XrXirType)type,sendable);
        if (compiled!=XR_XIR_OK) fprintf(stderr,"Result definition type=%u status=%u\n",type,compiled);
    }
    XrXirNominalTable *identities=NULL;
    if (compiled==XR_XIR_OK) compiled=xr_xir_compile_nominal_project(&owner->context,&table,NULL,&identities);
    XrXirValueStatus status=result_compile_status(compiled);
    if (status==XR_XIR_VALUE_OK) {
        types.nominals=identities;
        status=xr_xir_compile_type_arena_new(&owner->context,&types,&owner->arena);
    }
    xr_xir_compile_nominal_free(identities);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
    status=xr_xir_domain_new_budgeted(8388608,&controls,&owner->domain);
    if (status==XR_XIR_VALUE_OK) owner->admission=(XrXirValueAdmission){.arena=owner->arena,
        .domain=owner->domain,.work=128000000,.scratch_bytes=4194304};
    return status;
}
static void result_owner_free(ResultOwner *owner) {
    xr_xir_domain_drop(owner->domain); xr_xir_compile_type_arena_drop(owner->arena);
    xr_compile_resources_release(owner->context.resources); *owner=(ResultOwner){0};
    CHECK(!compiler_memory.live && !compiler_memory.bytes && !runtime_memory.live && !runtime_memory.bytes);
}
static void result_owner_ready(ResultOwner *owner) {
    XrXirValueStatus status=result_owner_new(owner);
    if (status!=XR_XIR_VALUE_OK) fprintf(stderr,"Result owner status=%u\n",status);
    CHECK(status==XR_XIR_VALUE_OK);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(owner->arena);
    for (uint32_t type=RESULT_CHANNEL_I64;type<=RESULT_CHANNEL_NESTED;++type) CHECK(xr_xir_type_is_channel(types,(XrXirType)type));
    CHECK(xr_xir_nominal_native_recv(types,(XrXirType)RESULT_I64,XR_XIR_I64));
    CHECK(xr_xir_nominal_native_recv(types,(XrXirType)RESULT_STRING,XR_XIR_STRING));
    CHECK(!xr_xir_nominal_native_recv(types,(XrXirType)RESULT_STRING,XR_XIR_I64));
    CHECK(xr_xir_nominal_native_send_result(types,(XrXirType)RESULT_SEND));
}
static void result_send(ResultOwner *owner, const XrXirValue *channel, XrXirValue *value) {
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(channel,value,&owner->admission,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SENT && result_empty(value));
}
static void result_variant(const XrXirValue *value, uint32_t expected, uint32_t fields) {
    uint32_t variant=UINT32_MAX; XrXirEnumBorrow borrow={0};
    CHECK(xr_xir_enum_variant(value,&variant)==XR_XIR_VALUE_OK && variant==expected);
    CHECK(xr_xir_enum_borrow(value,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==fields);
}
static void result_normal(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},value={0},output={0},field={0}; XrXirChannelState state=XR_XIR_CHANNEL_SENT;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,2,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_EMPTY);
    result_variant(&output,1,0); xr_xir_value_drop(&output);
    value=result_integer(42); result_send(&owner,&channel,&value);
    state=XR_XIR_CHANNEL_SENT;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_STRING,&owner.admission,&output,&state)==XR_XIR_VALUE_BAD_ARGUMENT &&
        result_empty(&output) && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    result_variant(&output,0,1);
    CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&field)==XR_XIR_VALUE_OK && field.type==XR_XIR_I64 && field.payload==42);
    xr_xir_value_drop(&field); xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED);
    result_variant(&output,3,0); xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_array_copy(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},array={0},message={0},output={0},field={0},number={0};
    XrXirValue input[]={result_integer(11),result_integer(22)},replacement=result_integer(99);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_ARRAY,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY,input,2,&owner.admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array,&message)==XR_XIR_VALUE_OK); result_send(&owner,&channel,&message);
    XrXirValuePlace place={(XrXirType)RESULT_ARRAY,&array.payload}; XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&replacement,&owner.admission,&fault)==XR_XIR_VALUE_OK);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_ARRAY_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&field)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&field,0,&owner.admission,&number,&fault)==XR_XIR_VALUE_OK && number.payload==11);
    xr_xir_value_drop(&number); xr_xir_value_drop(&field); xr_xir_value_drop(&output);
    xr_xir_value_drop(&array); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_escape(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},text={0},output={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(owner.domain,"result",6,&text)==XR_XIR_VALUE_OK); result_send(&owner,&channel,&text);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_STRING,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    xr_xir_value_drop(&channel); xr_xir_domain_drop(owner.domain); owner.domain=NULL;
    xr_xir_compile_type_arena_drop(owner.arena); owner.arena=NULL;
    xr_compile_resources_release(owner.context.resources); owner.context.resources=NULL;
    CHECK(xr_xir_value_valid(&output));
    CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&text)==XR_XIR_VALUE_OK);
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==6 && !memcmp(bytes,"result",6));
    xr_xir_value_drop(&text); xr_xir_value_drop(&output); result_owner_free(&owner);
}
static void result_deep_payload(ResultOwner *owner, XrXirValue *output) {
    XrXirValue inner={0},single={0},payload={0},text={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,1,&owner->admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(owner->domain,"nested",6,&text)==XR_XIR_VALUE_OK); result_send(owner,&inner,&text);
    CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY_CHANNEL,&inner,1,&owner->admission,&single)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)RESULT_DEEP_ARRAY,&single,1,&owner->admission,&payload)==XR_XIR_VALUE_OK);
    *output=payload; xr_xir_value_drop(&single); xr_xir_value_drop(&inner);
}
static void result_enqueue_deep(ResultOwner *owner, XrXirValue *channel) {
    XrXirValue payload={0}; result_deep_payload(owner,&payload); result_send(owner,channel,&payload);
}
static ResultOwner *result_interleave_owner;
static XrXirValue *result_interleave_channel;
static XrXirValue result_interleave_value;
static void result_other_receiver(void) {
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive(result_interleave_channel,&result_interleave_owner->admission,
        &result_interleave_value,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
}
static void result_changed_frontier(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},first={0},second={0},output={0}; XrXirChannelState state=XR_XIR_CHANNEL_SENT;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,2,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(owner.domain,"first",5,&first)==XR_XIR_VALUE_OK); result_send(&owner,&channel,&first);
    CHECK(xr_xir_string_new(owner.domain,"second",6,&second)==XR_XIR_VALUE_OK); result_send(&owner,&channel,&second);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);
    result_interleave_owner=&owner; result_interleave_channel=&channel;
    channel_cas_target=&storage->locked; channel_cas_count=0; channel_cas_pause_at=2; channel_cas_before=result_other_receiver;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_STRING,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RETRY && result_empty(&output) && !channel_cas_before && storage->buffer.count==1);
    channel_cas_target=NULL; channel_cas_pause_at=0;
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&result_interleave_value,&bytes,&length) && length==5 && !memcmp(bytes,"first",5));
    xr_xir_value_drop(&result_interleave_value); result_interleave_owner=NULL; result_interleave_channel=NULL;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_STRING,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    XrXirValue field={0}; CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&field)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&field,&bytes,&length) && length==6 && !memcmp(bytes,"second",6));
    xr_xir_value_drop(&field); xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_unit_payload(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},unit={0},output={0},field={0},array={0},message={0};
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_UNIT,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    result_send(&owner,&channel,&unit);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_UNIT,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    result_variant(&output,0,1);
    CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&field)==XR_XIR_VALUE_OK && result_empty(&field));
    CHECK(xr_xir_value_admit(&field,XR_XIR_UNIT,&owner.admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_new((XrXirType)RESULT_UNIT_ARRAY,&output,1,&owner.admission,&array)==XR_XIR_VALUE_OK);
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,0,&owner.admission,&message,&fault)==XR_XIR_VALUE_OK);
    result_variant(&message,0,1); xr_xir_value_drop(&message); xr_xir_value_drop(&output); xr_xir_value_drop(&channel);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_UNIT_RESULTS,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    result_send(&owner,&channel,&array);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_UNIT_RESULTS,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    CHECK(xr_xir_enum_get(&output,0,0,&owner.admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&array,0,&owner.admission,&message,&fault)==XR_XIR_VALUE_OK); result_variant(&message,0,1);
    CHECK(xr_xir_enum_get(&message,0,0,&owner.admission,&field)==XR_XIR_VALUE_OK && result_empty(&field));
    xr_xir_value_drop(&message); xr_xir_value_drop(&array); xr_xir_value_drop(&output);
    xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static size_t result_publication_fault(unsigned mode, size_t ordinal) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},text={0},output={0}; XrXirChannelState state=XR_XIR_CHANNEL_SENT;
    XrXirType channel_type=mode ? (XrXirType)RESULT_CHANNEL_DEEP : (XrXirType)RESULT_CHANNEL_STRING;
    XrXirType result_type=mode ? (XrXirType)RESULT_DEEP : (XrXirType)RESULT_STRING;
    CHECK(xr_xir_channel_new(channel_type,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    if (mode) result_enqueue_deep(&owner,&channel);
    else { CHECK(xr_xir_string_new(owner.domain,"preserved",9,&text)==XR_XIR_VALUE_OK); result_send(&owner,&channel,&text); }
    XirChannel *storage=(XirChannel *)object_pointer(&channel); int64_t payload=channel_slots(storage)[storage->buffer.read_index].payload;
    size_t begin=runtime_memory.attempts,live=runtime_memory.live,bytes=runtime_memory.bytes;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(owner.domain);
    if (ordinal!=SIZE_MAX) runtime_memory.fail_at=begin+ordinal;
    XrXirValueStatus status=xr_xir_channel_receive_result(&channel,result_type,&owner.admission,&output,&state);
    size_t sites=runtime_memory.attempts-begin; runtime_memory.fail_at=SIZE_MAX;
    if (ordinal!=SIZE_MAX) {
        CHECK(sites>ordinal && status==XR_XIR_VALUE_OOM && state==XR_XIR_CHANNEL_SENT && result_empty(&output));
        CHECK(storage->buffer.count==1 && channel_slots(storage)[storage->buffer.read_index].payload==payload);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(owner.domain);
        CHECK(failed.requested_bytes>before.requested_bytes && failed.work>before.work);
        CHECK(xr_xir_channel_receive_result(&channel,result_type,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
        XrXirDomainBudgetStats retry=xr_xir_domain_budget_stats(owner.domain);
        CHECK(retry.requested_bytes>failed.requested_bytes && retry.work>failed.work);
    } else CHECK(status==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    result_variant(&output,0,1); CHECK(!storage->buffer.count);
    xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner); return sites;
}
static void result_work_prefixes(void) {
    uint64_t required=0;
    for (uint64_t cut=UINT64_MAX;;++cut) {
        ResultOwner owner={0}; result_owner_ready(&owner); XrXirValue channel={0},message=result_integer(73),output={0};
        XrXirChannelState state=XR_XIR_CHANNEL_SENT;
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        result_send(&owner,&channel,&message); uint64_t begin=owner.admission.work;
        if (cut!=UINT64_MAX) owner.admission.work=cut;
        XrXirValueStatus status=xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state);
        if (cut==UINT64_MAX) { CHECK(status==XR_XIR_VALUE_OK); required=begin-owner.admission.work; CHECK(required && required<128); }
        else {
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) {
                XirChannel *storage=(XirChannel *)object_pointer(&channel);
                CHECK(result_empty(&output) && state==XR_XIR_CHANNEL_SENT && storage->buffer.count==1);
                CHECK(channel_slots(storage)[storage->buffer.read_index].payload==73);
            }
        }
        xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner);
        if (cut==required) break;
    }
    printf("Channel Recv publication work exact=%" PRIu64 " prefixes=%" PRIu64 " physical=0\n",required,required+1);
}
#include "xir_channel_result_metadata_cases.h"
#include "xir_channel_send_result_cases.h"
#include "xir_channel_try_publication_cases.h"
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal"); CHECK(argc==1 || normal);
    result_normal(); result_array_copy(); result_escape(); result_work_prefixes(); result_changed_frontier(); result_unit_payload();
    result_raw_metadata(); result_projected_metadata();
    result_send_outcomes(); result_send_rejections(); result_send_unit(); result_send_work();
    try_copy_work();try_copy_rejections();
    if (!normal) {
        size_t copy_sites=try_copy_fault(SIZE_MAX);CHECK(copy_sites && copy_sites<128);
        for (size_t ordinal=0;ordinal<copy_sites;++ordinal) CHECK(try_copy_fault(ordinal)>ordinal);
        printf("Channel ordinary trySend publication ordinals=%zu source/queue preserved physical0 same-owner recovery PASS\n",copy_sites);
        size_t send_sites=result_send_publication_fault(SIZE_MAX); CHECK(send_sites && send_sites<128);
        for (size_t ordinal=0;ordinal<send_sites;++ordinal) CHECK(result_send_publication_fault(ordinal)>ordinal);
        printf("Channel SendResult actual publication allocator ordinals=%zu queue-and-input-preserved physical=0 same-owner recovery cumulative=PASS\n",send_sites);
        for (unsigned mode=0;mode<2;++mode) {
            size_t sites=result_publication_fault(mode,SIZE_MAX); CHECK(sites && sites<128);
            for (size_t ordinal=0;ordinal<sites;++ordinal) CHECK(result_publication_fault(mode,ordinal)>ordinal);
            printf("Channel Recv actual publication mode=%u allocator ordinals=%zu queue-preserved physical=0 same-owner recovery cumulative=PASS\n",mode,sites);
        }
    }
    puts("Channel owned Recv candidate PASS; public execution and waits not covered"); return 0;
}
