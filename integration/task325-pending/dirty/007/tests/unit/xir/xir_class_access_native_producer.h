/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_native_producer.h - Normal emission of authentic Class access programs
 */
#ifndef XIR_CLASS_ACCESS_NATIVE_PRODUCER_H
#define XIR_CLASS_ACCESS_NATIVE_PRODUCER_H
#include "xir/xxir_emit_c.h"

typedef struct AccessGeneratedFixture {
    const XrXirProgramSpec *program;
    const uint8_t *native;
    CellSourceEntries entries;
    uint32_t count;
} AccessGeneratedFixture;

typedef struct AccessGeneratedOutput {
    SourceFixtureOwner owner;
    XrXirCSource source;
    CellSourceEntries entries;
    uint8_t *native;
    uint32_t count;
} AccessGeneratedOutput;

static bool access_native_named(const XrXirFunction *function,const char *name) {
    size_t length=strlen(name);
    return function->name_length==length && !memcmp(function->name,name,length);
}
static uint8_t *access_native_routes(const XrXirCompileContext *context,const XrXirModule *module,bool alias) {
    void *memory=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,1,&memory)==XR_COMPILE_RESOURCE_OK);
    uint8_t *routes=memory;
    unsigned bump=0,adjust=0,inner=0,counter=0,worker=0,run=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        CHECK(xr_compile_resources_work(context->resources,1)==XR_COMPILE_RESOURCE_OK);
        if (access_native_named(function,"bump")) {routes[f]=1;++bump;}
        if (access_native_named(function,"adjust")) {
            CHECK(identity->method_kind==XR_XIR_READ_METHOD && identity->nominal_owner);
            routes[f]=1;++adjust;
        }
        if (access_native_named(function,"inner")) {routes[f]=1;++inner;}
        if (access_native_named(function,"cellCounter")) {routes[f]=1;++counter;}
        if (access_native_named(function,"worker")) {CHECK(!routes[f]);++worker;}
        if (access_native_named(function,"run")) {CHECK(!routes[f] && identity->exported);++run;}
    }
    CHECK(run==1);
    if (alias) CHECK(!bump && !adjust && inner==1 && counter==1 && !worker);
    else CHECK(bump==1 && adjust==1 && !inner && !counter && worker==1);
    return routes;
}
static void access_generated_prepare(unsigned fixture,AccessGeneratedOutput *output) {
    CHECK(fixture<5 && !output->owner.context.resources && !output->source.text);
    const AccessSourceCase *input=&access_source_cases[fixture];
    source_fixture_owner_new(&output->owner);
    XrXirArtifact *lowered=NULL;
    XrXirStatus status=cell_source_lower(&output->owner.context,input->name,&lowered,false);
    if (input->rejected) {
        CHECK(status==XR_XIR_BAD_TYPE && !lowered && !output->source.text && !output->source.length);
        printf("CLASS_ACCESS_GENERATE_REJECT fixture=%s status=%u artifact=0 C=0\n",input->name,(unsigned)status);
        source_fixture_owner_free(&output->owner);return;
    }
    CHECK(status==XR_XIR_OK && lowered);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(module && module->stage==XR_XIR_LOWERED && module->declarations && module->function_count);
    output->entries=(CellSourceEntries){cell_source_entry(module,"run",true),
        cell_source_entry(module,"cellCounter",false),cell_source_entry(module,"cellFactory",false)};
    CHECK(output->entries.factory==UINT32_MAX && (output->entries.counter!=UINT32_MAX)==input->alias);
    output->count=module->function_count;
    output->native=access_native_routes(&output->owner.context,module,input->alias);
    char prefix[64];CHECK(snprintf(prefix,sizeof(prefix),"class_access_generated_%u",fixture)>0);
    CHECK(xr_xir_compile_emit_c(lowered,prefix,4194304,&output->source)==XR_XIR_OK);
    CHECK(output->source.text && output->source.length && !output->source.text[output->source.length]);
    CHECK(!strstr(output->source.text,"({") && strstr(output->source.text,"xr_xir_instance_cell_project_at("));
    CHECK(strstr(output->source.text,"xr_xir_instance_cell_read_at(") &&
        strstr(output->source.text,"xr_xir_instance_cell_write_at("));
    /* The snapshot is an ordinary Array value, read after the Class field. */
    if (fixture==1) CHECK(strstr(output->source.text,"xr_xir_class_get(") &&
        strstr(output->source.text,"xr_xir_instance_array_read("));
    xr_xir_compile_artifact_free(lowered);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
}
static void access_generated_header(FILE *file,const AccessGeneratedOutput outputs[5]) {
    CHECK(fputs("/* Authentic CGen specifications and dynamically resolved Source entries. */\n",file)>=0);
    for (unsigned i=0;i<5;++i) if (!access_source_cases[i].rejected) {
        const AccessGeneratedOutput *output=&outputs[i];
        CHECK(fprintf(file,"XR_DATA const XrXirProgramSpec class_access_generated_%u_program;\n"
            "static const uint8_t class_access_generated_%u_native[%u]={",i,i,output->count)>0);
        for (uint32_t f=0;f<output->count;++f)
            CHECK(fprintf(file,"%s%uu",f?",":"",(unsigned)output->native[f])>0);
        CHECK(fputs("};\n",file)>=0);
    }
    CHECK(fputs("static const AccessGeneratedFixture access_generated_fixtures[5]={\n",file)>=0);
    for (unsigned i=0;i<5;++i) {
        const AccessGeneratedOutput *output=&outputs[i];
        if (access_source_cases[i].rejected) CHECK(fputs("    {NULL,NULL,{UINT32_MAX,UINT32_MAX,UINT32_MAX},0},\n",file)>=0);
        else CHECK(fprintf(file,"    {&class_access_generated_%u_program,class_access_generated_%u_native,"
            "{%uu,%uu,%uu},%uu},\n",i,i,output->entries.run,output->entries.counter,output->entries.factory,output->count)>0);
    }
    CHECK(fputs("};\n",file)>=0);
}
static int access_write_c(int argc,char **argv) {
    if (argc!=6 || strcmp(argv[1],"--write-c")) return 2;
    const char *names[4]={"class_access_0.c","class_access_1.c","class_access_3.c","class_access_generated.h"};
    for (unsigned i=2;i<6;++i) {
        if (!argv[i][0]) return 2;
        const char *name=argv[i];
        for (const char *at=name;*at;++at) if (*at=='/' || *at=='\\') name=at+1;
        if (strcmp(name,names[i-2])) return 2;
        for (unsigned prior=2;prior<i;++prior) if (!strcmp(argv[i],argv[prior])) return 2;
    }
    CHECK(!access_compile_recording && !cell_runtime_recording && !cell_runtime_fault_count);
    AccessGeneratedOutput outputs[5]={0};
    /* Every admission and emission succeeds before any declared output opens. */
    for (unsigned i=0;i<5;++i) access_generated_prepare(i,&outputs[i]);
    unsigned path=2;
    for (unsigned i=0;i<5;++i) if (!access_source_cases[i].rejected) {
        FILE *file=fopen(argv[path++],"wb");CHECK(file);
        CHECK(fwrite(outputs[i].source.text,1,outputs[i].source.length,file)==outputs[i].source.length && !fclose(file));
    }
    CHECK(path==5);
    FILE *header=fopen(argv[5],"wb");CHECK(header);access_generated_header(header,outputs);CHECK(!fclose(header));
    for (unsigned i=0;i<5;++i) if (!access_source_cases[i].rejected) {
        printf("CLASS_ACCESS_GENERATE fixture=%s functions=%u Cbytes=%zu Source=1 CheckedWrite=1 CheckedRead=1 Specialize=1 Lower=1 Emit=1 fullFI=NOT_RUN runtimeExecution=NOT_RUN\n",
            access_source_cases[i].name,outputs[i].count,outputs[i].source.length);
        xr_compile_resources_free(outputs[i].native);xr_xir_compile_c_source_free(&outputs[i].source);
        source_fixture_owner_free(&outputs[i].owner);
    }
    access_physical_zero();CHECK(!cell_runtime_observer_live);return 0;
}
#endif // XIR_CLASS_ACCESS_NATIVE_PRODUCER_H
