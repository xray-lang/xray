/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_array_error_emit.c - Real generated entries for owned Task error arrays
 */
#define main unit_native_reference_main
#include "test_xir_task_unit_native_producer.c"
#undef main
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"array","nested","strings"};char path[2048],symbol[128];
    CHECK(snprintf(path,sizeof(path),"%s/task_array_error_generated.h",argv[1])>0);
    FILE *header=fopen(path,"wb");CHECK(header);
    uint32_t entries[3]={0};
    for(unsigned i=0;i<3;++i) {
        UnitNativeOracle oracle={names[i],"handle",true,false,true,0};
        XrXirArtifact *lowered=unit_native_lower(&oracle,&entries[i]);
        CHECK(snprintf(symbol,sizeof(symbol),"task_array_error_%s",names[i])>0);
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,symbol,4194304,&source)==XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);CHECK(source.text&&!strstr(source.text,"({"));
        CHECK(snprintf(path,sizeof(path),"%s/%s.c",argv[1],symbol)>0);
        FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length&&!fclose(file));
        CHECK(fprintf(header,"XR_DATA const XrXirProgramSpec %s_program;\n",symbol)>0);
        xr_xir_compile_c_source_free(&source);
    }
    CHECK(fprintf(header,"static const uint32_t array_error_entries[3]={%uu,%uu,%uu};\n",entries[0],entries[1],entries[2])>0);
    CHECK(fprintf(header,"static const XrXirProgramSpec *const array_error_specs[3]={&task_array_error_array_program,&task_array_error_nested_program,&task_array_error_strings_program};\n")>0);
    CHECK(!fclose(header));effects_source_owners_free();return 0;
}
