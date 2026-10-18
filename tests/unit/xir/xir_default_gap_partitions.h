/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_gap_partitions.h - Complete whole-graph compiler fault reports
 */
#ifndef XIR_DEFAULT_GAP_PARTITIONS_H
#define XIR_DEFAULT_GAP_PARTITIONS_H
static FILE *gap_compiler_report_file;
static size_t gap_compiler_sites,gap_compiler_points,gap_compiler_axes;
static size_t gap_compiler_number(const char *text) {
    size_t value=0;CHECK(text && *text);
    for(const unsigned char *p=(const unsigned char *)text;*p;++p){
        CHECK(*p>='0' && *p<='9' && value<=(SIZE_MAX-(*p-'0'))/10);value=value*10+(*p-'0');
    }
    return value;
}
static void gap_compiler_report(const DefaultCompilerFaultEvent *event) {
    CHECK(event && event->kind<=2 && event->variant==0);
    const char *kind=event->kind==0?"baseline":event->kind==1?"point":"axis";
    if(!event->kind){gap_compiler_sites=event->sites;default_packet_path=default_c_path=NULL;}
    else if(event->kind==1)++gap_compiler_points;else ++gap_compiler_axes;
    XrCompileResourceLimits caps=default_limits();
    CHECK(fprintf(gap_compiler_report_file,
        "{\"kind\":\"%s\",\"mode\":%u,\"sites\":%zu,\"ordinal\":%zu,\"axis\":%u,"
        "\"status\":%u,\"attempts\":%zu,\"injected\":%s,\"empty_output\":%s,\"ledger_oom\":%s,"
        "\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,"
        "\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu,"
        "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,\"runtime_blocks\":%zu,\"runtime_bytes\":%zu,\"runtime_owned\":%u}\n",
        kind,DEFAULT_MODE,event->sites,event->ordinal,event->axis,(unsigned)event->status,event->attempts,
        event->injected?"true":"false",event->empty_output?"true":"false",event->ledger_oom?"true":"false",
        (unsigned long long)event->exact.allocated_bytes,(unsigned long long)event->exact.peak_bytes,
        (unsigned long long)event->exact.work,(unsigned long long)caps.allocated_bytes,
        (unsigned long long)caps.live_bytes,(unsigned long long)caps.work,instance_compile_live,
        instance_compile_bytes,runtime_live,runtime_bytes,(unsigned)(runtime_owned!=NULL))>0);
    CHECK(!fflush(gap_compiler_report_file));
}
static int default_gap_compiler_command(int argc,char **argv) {
    unsigned phase=0;size_t index=0,workers=1,expected=0;
    if(!strcmp(argv[1],"--compiler-baseline"))CHECK(argc==5);
    else if(!strcmp(argv[1],"--compiler-shard")){
        CHECK(argc==8);phase=1;index=gap_compiler_number(argv[4]);
        workers=gap_compiler_number(argv[5]);expected=gap_compiler_number(argv[6]);
    }else {CHECK(!strcmp(argv[1],"--compiler-bounds") && argc==6);phase=2;expected=gap_compiler_number(argv[4]);}
    CHECK(workers>0 && workers<=8 && index<workers && (phase==0 || (expected>0 && expected<20000)));
    gap_compiler_report_file=fopen(argv[argc-1],"wb");CHECK(gap_compiler_report_file);
    uint8_t *bytes=default_gap_inputs(argv[2],argv[3]);
    default_compiler_partition=(DefaultCompilerPartition){true,phase,index,workers,expected,gap_compiler_report};
    /* Only the fresh whole-graph baseline writes this process's own artifacts.
     * Fault ordinals retain the original no-output-file compiler execution. */
    gap_seal_faults(default_build);
    CHECK(fprintf(gap_compiler_report_file,
        "{\"kind\":\"complete\",\"mode\":%u,\"index\":%zu,\"workers\":%zu,\"sites\":%zu,"
        "\"points\":%zu,\"axes\":%zu,\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,"
        "\"runtime_blocks\":%zu,\"runtime_bytes\":%zu,\"runtime_owned\":%u}\n",
        DEFAULT_MODE,index,workers,gap_compiler_sites,gap_compiler_points,gap_compiler_axes,
        instance_compile_live,instance_compile_bytes,runtime_live,runtime_bytes,(unsigned)(runtime_owned!=NULL))>0);
    CHECK(!fclose(gap_compiler_report_file));gap_compiler_report_file=NULL;free(bytes);return 0;
}
#endif // XIR_DEFAULT_GAP_PARTITIONS_H
