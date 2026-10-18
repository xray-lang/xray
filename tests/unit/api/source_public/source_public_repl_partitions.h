/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_partitions.h - Process-owned exact fault partitions
 */
#ifndef SOURCE_PUBLIC_REPL_PARTITIONS_H
#define SOURCE_PUBLIC_REPL_PARTITIONS_H
static void public_repl_json_string(FILE *file,const char *text) {
    CHECK(fputc('"',file)!=EOF);
    for(const unsigned char *p=(const unsigned char *)text;*p;++p) {
        if(*p=='"' || *p=='\\')CHECK(fputc('\\',file)!=EOF && fputc(*p,file)!=EOF);
        else if(*p<32)CHECK(fprintf(file,"\\u%04x",(unsigned)*p)>0);
        else CHECK(fputc(*p,file)!=EOF);
    }
    CHECK(fputc('"',file)!=EOF);
}
static size_t public_repl_partition_number(const char *text) {
    CHECK(text && *text);
    size_t result=0;
    for(const unsigned char *p=(const unsigned char *)text;*p;++p) {
        CHECK(*p>='0' && *p<='9' && result<=((UINT64_C(64)<<20)-(*p-'0'))/10);
        result=result*10+(*p-'0');
    }
    return result;
}
static int public_repl_partition_command(int argc,char **argv) {
    CHECK(argc==4 || argc==6);
    bool inputs=!strcmp(argv[1],"--repl-fi-inputs");
    bool baseline=!strcmp(argv[1],"--repl-fi-baseline");
    bool partition=!strcmp(argv[1],"--repl-fi-partition");
    bool single=!strcmp(argv[1],"--repl-fi-point");
    CHECK((argc==4 && (inputs || baseline)) || (argc==6 && (partition || single)));
    const char *mode=argv[2];
    CHECK(!strcmp(mode,"history") || !strcmp(mode,"inspection") || !strcmp(mode,"rooted") ||
        !strcmp(mode,"file") || !strcmp(mode,"array") || !strcmp(mode,"write"));
    public_repl_inspection_case=!strcmp(mode,"inspection");
    public_repl_rooted_case=!strcmp(mode,"rooted");
    public_repl_file_case=!strcmp(mode,"file");
    public_repl_array_case=!strcmp(mode,"array");
    public_repl_write_case=!strcmp(mode,"write");
    public_repl_partition.partition=partition || single;
    public_repl_partition.single_point=single;
    if(partition || single) {
        size_t selected=public_repl_partition_number(argv[3]);
        public_repl_partition.expected=public_repl_partition_number(argv[4]);
        CHECK(public_repl_partition.expected>0);
        CHECK(single ? selected>0 && selected<=public_repl_partition.expected : selected<PUBLIC_REPL_FAULT_WORKERS);
        public_repl_partition.index=single ? selected-1 : selected;
    }
    FILE *report=fopen(argv[argc-1],"wb");CHECK(report);
    public_repl_partition.report=report;
    CHECK(fputs("{\"kind\":\"inputs\",\"mode\":",report)>=0);
    public_repl_json_string(report,mode);
    CHECK(fputs(",\"stdlib\":",report)>=0);public_repl_json_string(report,XR_PUBLIC_STDLIB);
    CHECK(fputs(",\"rooted\":",report)>=0);public_repl_json_string(report,XR_PUBLIC_REPL_FIXTURE);
    CHECK(fputs(",\"file\":",report)>=0);public_repl_json_string(report,XR_PUBLIC_REPL_FILE_FIXTURE);
    CHECK(fprintf(report,",\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu}\n",
        (unsigned long long)public_repl_fault_limits.allocated_bytes,
        (unsigned long long)public_repl_fault_limits.live_bytes,
        (unsigned long long)public_repl_fault_limits.work)>0);
    if(!inputs)public_repl_faults(baseline);
    CHECK(fclose(report)==0);
    public_repl_partition.report=NULL;
    return 0;
}
#endif // SOURCE_PUBLIC_REPL_PARTITIONS_H
