/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_value_format_storage.c - Display borrows real packed Array owners
 */
#define main original_value_allocation_main
#include "test_xir_value_allocations.c"
#undef main
#include "xir/xxir_format.c"

typedef struct FormatCapture { char bytes[4096]; size_t used, writes, fail_at; } FormatCapture;
static int capture_format(void *context, const void *bytes, size_t length) {
    FormatCapture *capture = context;
    if (capture->writes++ == capture->fail_at || length > sizeof(capture->bytes)-capture->used) return 0;
    if (length) memcpy(capture->bytes+capture->used, bytes, length);
    capture->used += length; return 1;
}
static void format_exact(const XrXirValue *value, const char *expected, size_t length) {
    size_t before = calls, owners = live, references = atomic_load(&object_pointer(value)->references);
    XrValueFormatReader reader = xr_xir_value_format_reader(value);
    FormatCapture baseline = {.fail_at=SIZE_MAX};
    closed_fail_all = true;
    CHECK(xr_value_format_value(reader, (XrValueFormatNode){value,0, NULL},
        (XrValueFormatSink){&baseline,capture_format}, 0));
    CHECK(baseline.used==length && !memcmp(baseline.bytes,expected,length));
    for (size_t site=0; site<baseline.writes; ++site) {
        FormatCapture capture = {.fail_at=site};
        CHECK(!xr_value_format_value(reader,(XrValueFormatNode){value,0, NULL},
            (XrValueFormatSink){&capture,capture_format},0));
        CHECK(capture.writes==site+1);
    }
    closed_fail_all = false;
    CHECK(calls==before && live==owners && atomic_load(&object_pointer(value)->references)==references);
}
static XrXirTypeArena *format_arena(void) {
    const XrXirNominalVariant variants[] = {{{"Empty",5},0,0},{{"Pair",4},0,2}};
    const XrXirNominalFieldIdentity fields[] = {{{"number",6},0},{{"text",4},0}};
    const XrXirType field_types[] = {XR_XIR_I16,XR_XIR_STRING};
    XrXirNominalIdentity identity = {.module={"format",6},.name={"Choice",6},.exported=1,
        .fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=2};
    XrXirNominalTable table = {.count=1,.identities=&identity};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=field_types,.field_count=2}},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)261},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)263},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U64}
    };
    XrXirTypes types = {nodes,10,&table,NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(65536,0,10000),&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void format_received_storage(void) {
    const XrXirNominalVariant variant = {{"Items",5},0,1};
    const XrXirNominalFieldIdentity field = {{"items",5},0};
    XrXirNominalIdentity identity = {.module={"format",6},.name={"Box",3},.exported=1,
        .fields=&field,.field_count=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=&variant,.variant_count=1};
    XrXirNominalTable table = {.count=1,.identities=&identity};
    XrXirType source_field=(XrXirType)256, target_field=(XrXirType)257;
    XrXirTypeNode source_nodes[] = {
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=&source_field,.field_count=1}},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)259}};
    XrXirTypeNode target_nodes[6] = {{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL}};
    memcpy(target_nodes+1,source_nodes,sizeof(source_nodes));
    target_nodes[2].element=(XrXirType)257;
    target_nodes[3].nominal.fields=&target_field;
    target_nodes[4].element=(XrXirType)259; target_nodes[5].element=(XrXirType)260;
    XrXirTypes pools[]={{source_nodes,5,&table,NULL},{target_nodes,6,&table,NULL}};
    ValueCompileOwner compiler={0}; XrXirTypeArena *source=NULL,*target=NULL;
    CHECK(value_compile_owner_new(&compiler,value_compile_limits(262144,262144,1000000),65536)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&pools[0],&source)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot roots[]={{(XrXirType)257,(XrXirType)258},
        {(XrXirType)260,(XrXirType)261}};
    CHECK(xir_compile_type_arena_receive(&compiler.context,target,source,roots,2)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,1000000,65536};
    XrXirValue scalar={XR_XIR_I8,0,-17}, inner={0}, outer={0}, boxed={0}, optional={0}, packed={0};
    CHECK(xr_xir_array_new((XrXirType)256,&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    admission.arena=target;
    CHECK(xr_xir_array_new((XrXirType)258,&inner,1,&admission,&outer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)259,0,&inner,1,&admission,&boxed)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)260,&boxed,&admission,&optional)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)261,&optional,1,&admission,&packed)==XR_XIR_VALUE_OK);
    XrValueFormatReader reader=xr_xir_value_format_reader(&outer); XrValueFormatNode child={0}, leaf={0};
    CHECK(reader.child(reader.context,(XrValueFormatNode){&outer,0,NULL},0,&child) && child.owner==target);
    CHECK(reader.child(reader.context,child,0,&leaf) && leaf.owner==source && leaf.type==XR_XIR_I8);
    uint32_t source_refs=atomic_load(&source->references), target_refs=atomic_load(&target->references);
    uint32_t inner_refs=atomic_load(&object_pointer(&inner)->references);
    format_exact(&outer,"[[-17]]",7); format_exact(&packed,"[Box.Items([-17])]",18);
    CHECK(inner.type==256 && xr_xir_value_arena(&inner)==source);
    CHECK(atomic_load(&object_pointer(&inner)->references)==inner_refs);
    CHECK(atomic_load(&source->references)==source_refs && atomic_load(&target->references)==target_refs);
    value_compile_owner_release(&compiler);
    xr_xir_compile_type_arena_drop(source); xr_xir_compile_type_arena_drop(target); xr_xir_domain_drop(domain);
    format_exact(&outer,"[[-17]]",7); format_exact(&packed,"[Box.Items([-17])]",18);
    xr_xir_value_drop(&packed); xr_xir_value_drop(&optional); xr_xir_value_drop(&boxed);
    xr_xir_value_drop(&outer); xr_xir_value_drop(&inner);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
}
int main(void) {
    format_received_storage();
    XrXirTypeArena *arena = format_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,65536,65536};
    XrXirValue numbers[] = {{XR_XIR_I8,0,-128},{XR_XIR_I8,0,127}}, array={0}, nested={0};
    CHECK(xr_xir_array_new((XrXirType)256,numbers,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)258,&array,1,&admission,&nested)==XR_XIR_VALUE_OK);
    format_exact(&array,"[-128, 127]",11); format_exact(&nested,"[[-128, 127]]",13);
    XrValueFormatReader reader = xr_xir_value_format_reader(&array); XrValueFormatNode child={0};
    CHECK(reader.child(reader.context,(XrValueFormatNode){&array,0, NULL},1,&child));
    CHECK(child.type==XR_XIR_I8 && child.value==((XirArray *)object_pointer(&array))->data+1);
    CHECK(!reader.child(reader.context,(XrValueFormatNode){&array,0, NULL},2,&child));
    XrXirValue string={0}, strings={0};
    CHECK(xr_xir_string_new(domain,"A\0\xE4\xB8\xAD",5,&string)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)257,&string,1,&admission,&strings)==XR_XIR_VALUE_OK);
    static const char quoted[]="[\"A\0\xE4\xB8\xAD\"]";
    format_exact(&strings,quoted,sizeof(quoted)-1);
    XrXirValue fields[] = {{XR_XIR_I16,0,-32768},string}, cases[2]={{0}}, enums={0};
    CHECK(xr_xir_enum_new((XrXirType)259,0,NULL,0,&admission,&cases[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)259,1,fields,2,&admission,&cases[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)260,cases,2,&admission,&enums)==XR_XIR_VALUE_OK);
    static const char enum_text[]="[Choice.Empty, Choice.Pair(-32768, \"A\0\xE4\xB8\xAD\")]";
    format_exact(&enums,enum_text,sizeof(enum_text)-1);
    XrXirValue options[2]={{0}}, nullable={0}, integer={XR_XIR_I64,0,INT64_MIN};
    CHECK(xr_xir_nullable_new((XrXirType)261,NULL,&admission,&options[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)261,&integer,&admission,&options[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)262,options,2,&admission,&nullable)==XR_XIR_VALUE_OK);
    format_exact(&nullable,"[null, -9223372036854775808]",28);
    XrXirValue optional_enum={0}, optional_enums={0};
    CHECK(xr_xir_nullable_new((XrXirType)263,&cases[1],&admission,&optional_enum)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)264,&optional_enum,1,&admission,&optional_enums)==XR_XIR_VALUE_OK);
    static const char optional_text[]="[Choice.Pair(-32768, \"A\0\xE4\xB8\xAD\")]";
    format_exact(&optional_enums,optional_text,sizeof(optional_text)-1);
    XrXirValue maximum={XR_XIR_U64,0,-1}, unsigneds={0};
    CHECK(xr_xir_array_new((XrXirType)265,&maximum,1,&admission,&unsigneds)==XR_XIR_VALUE_OK);
    format_exact(&unsigneds,"[18446744073709551615]",22);
    XrXirValue repeated={0};
    CHECK(xr_xir_array_repeat((XrXirType)256,33,&numbers[1],&admission,&repeated)==XR_XIR_VALUE_OK);
    static const char clipped[]="[127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, ...(1 more)]";
    format_exact(&repeated,clipped,sizeof(clipped)-1);
    uint32_t string_refs=atomic_load(&object_pointer(&string)->references);
    atomic_store(&object_pointer(&string)->references,UINT32_MAX);
    uint32_t arena_refs=atomic_load(&arena->references);
    atomic_store(&arena->references,UINT32_MAX);
    format_exact(&strings,quoted,sizeof(quoted)-1);
    CHECK(atomic_load(&object_pointer(&string)->references)==UINT32_MAX && atomic_load(&arena->references)==UINT32_MAX);
    atomic_store(&object_pointer(&string)->references,string_refs); atomic_store(&arena->references,arena_refs);
    reader.context=NULL; FormatCapture rejected={.fail_at=SIZE_MAX};
    CHECK(!xr_value_format_value(reader,(XrValueFormatNode){&array,0, NULL},
        (XrValueFormatSink){&rejected,capture_format},0) && !rejected.writes);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    format_exact(&nested,"[[-128, 127]]",13);
    format_exact(&enums,enum_text,sizeof(enum_text)-1);
    xr_xir_value_drop(&optional_enums); xr_xir_value_drop(&optional_enum);
    xr_xir_value_drop(&unsigneds); xr_xir_value_drop(&nullable);
    for (size_t i=0;i<2;++i) { xr_xir_value_drop(&options[i]); xr_xir_value_drop(&cases[i]); }
    xr_xir_value_drop(&enums); xr_xir_value_drop(&strings); xr_xir_value_drop(&string);
    xr_xir_value_drop(&nested); xr_xir_value_drop(&array);
    xr_xir_value_drop(&repeated);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Packed Array formatting: exact bytes, borrowed offsets, zero allocations, sink failures and owner lifetime passed");
    return 0;
}
