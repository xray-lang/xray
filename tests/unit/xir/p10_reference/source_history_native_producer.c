/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_native_producer.c - Actual Source generations into portable C
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_source_fixture_owner.h"
#include "source_history_cell_fixture.h"
static const char *history_mode_name(unsigned kind) {
    static const char *const names[]={"read","write","array","array_const","class","class_array","array_equal","class_ref","class_inline_ref"};
    CHECK(kind<9);return names[kind];
}
static XrXirOutputStatus output_once(void *context,const XrXirOutputGroup *group) {
    unsigned *calls=context;
    CHECK(!*calls && group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    ++*calls;return XR_XIR_OUTPUT_OK;
}
static void produce_mode(const char *directory,FILE *header,unsigned kind) {
    bool mutating=kind==1 || kind==2 || kind==4 || kind==5 || kind==7 || kind==8;
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"history-check-test",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;
    CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    unsigned calls=0;runtime.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=output_once,.context=&calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime,&instance)==XR_XIR_CALL_READY);
    for (unsigned generation=1;generation<=4;++generation) {
        const char *source=source_history_cell_input(generation,kind);
        XrXirSourceText text={NULL,source,strlen(source)};uint64_t serial=0;
        CHECK(xr_xir_compile_source_generation_prepare(owner,&text,&serial,NULL)==XR_XIR_OK && serial==generation);
        XrXirSourceGenerationView view={0};
        CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
        XrXirArtifact *closed=NULL,*lowered=NULL;
        CHECK(xr_xir_compile_specialize(view.result->checked,&closed,NULL)==XR_XIR_OK);
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(closed);
        char prefix[80],path[4096];const char *mode=history_mode_name(kind);
        int n=snprintf(prefix,sizeof(prefix),"source_history_%s_%u",mode,generation);CHECK(n>0 && (size_t)n<sizeof(prefix));
        n=snprintf(path,sizeof(path),"%s/%s.c",directory,prefix);CHECK(n>0 && (size_t)n<sizeof(path));
        XrXirCSource emitted={0};
        CHECK(xr_xir_compile_emit_c(lowered,prefix,4*1024*1024,&emitted)==XR_XIR_OK);
        FILE *file=fopen(path,"wb");CHECK(file && fwrite(emitted.text,1,emitted.length,file)==emitted.length && !fclose(file));
        xr_xir_compile_c_source_free(&emitted);
        CHECK(fprintf(header,"extern const XrXirProgramSpec %s_program;\n",prefix)>0);
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        const char *name=generation==1 ? "seed" : generation==4 ? "fail" : "answer";
        uint32_t entry=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f)
            if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) {
                CHECK(entry==UINT32_MAX);entry=f;
            }
        CHECK(entry!=UINT32_MAX);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
        CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,program)==XR_XIR_OK);
        uint64_t candidate=0;
        CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&candidate)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        if (generation==4) {
            CHECK(status==XR_XIR_CALL_ASSERTION);
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
        } else {
            CHECK(status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if (kind==7 && generation>1) {
                const char *expected=generation==2 ? "ax1" : "ax1x1",*bytes=NULL;size_t length=0;
                CHECK(xr_xir_string_view(&value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
            } else CHECK(value.type==XR_XIR_I64 && value.payload==(generation==1 ? 0 : mutating && generation==3 ? 43 : 42));
            xr_xir_value_drop(&value);
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
        }
        xr_xir_compile_program_drop(program);
    }
    CHECK(calls==1);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);source_fixture_owner_free(&compiler);
}
int main(int argc,char **argv) {
    CHECK(argc==2);char path[4096];int n=snprintf(path,sizeof(path),"%s/source_history_native_generated.h",argv[1]);
    CHECK(n>0 && (size_t)n<sizeof(path));FILE *header=fopen(path,"wb");CHECK(header);
    CHECK(fputs("#ifndef SOURCE_HISTORY_NATIVE_GENERATED_H\n#define SOURCE_HISTORY_NATIVE_GENERATED_H\n#include \"xir/xxir_program.h\"\n",header)>=0);
    for (unsigned mode=0;mode<9;++mode) produce_mode(argv[1],header,mode);
    CHECK(fputs("static const XrXirProgramSpec *const source_history_native_programs[9][4]={\n",header)>=0);
    for (unsigned mode=0;mode<9;++mode) {
        CHECK(fputs("{",header)>=0);
        for (unsigned generation=1;generation<=4;++generation)
            CHECK(fprintf(header,"%s&source_history_%s_%u_program",generation==1 ? "" : ",",history_mode_name(mode),generation)>0);
        CHECK(fputs(mode==8 ? "}\n" : "},\n",header)>=0);
    }
    CHECK(fputs("};\n#endif\n",header)>=0 && !fclose(header));
    return 0;
}
