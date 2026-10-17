/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_allocation_case_workers.h - Deterministic complete allocation case partitions
 *
 * KEY CONCEPT:
 *   Each selected case executes its original full fault loops and lifetime
 *   checks. A census measures the unchanged input before any partition runs.
 */
#ifndef XIR_ALLOCATION_CASE_WORKERS_H
#define XIR_ALLOCATION_CASE_WORKERS_H

enum { ALLOCATION_WORKER_CASES=67 };

static size_t allocation_worker_number(const char *text) {
    CHECK(text && *text);size_t value=0;
    for(const char *p=text;*p;++p) {
        CHECK(*p>='0' && *p<='9');unsigned digit=(unsigned)(*p-'0');
        CHECK(value<=(SIZE_MAX-digit)/10);value=value*10+digit;
    }
    return value;
}
static void allocation_worker_hex(const char *text,size_t length) {
    CHECK(text && strlen(text)==length);
    for(size_t i=0;i<length;++i)CHECK((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f'));
}
static void allocation_worker_begin(const char *mode,size_t index,size_t workers,const char *nonce) {
    const char *input=getenv("XRAY_ALLOCATION_INPUT_SHA256");
    const char *binary=getenv("XRAY_ALLOCATION_BINARY_SHA256");
    const char *config=getenv("XRAY_ALLOCATION_CONFIG_SHA256");
    allocation_worker_hex(nonce,32);allocation_worker_hex(input,64);
    allocation_worker_hex(binary,64);allocation_worker_hex(config,64);
    unsigned asan=0,ubsan=0;
#if defined(XR_BUILD_ASAN)
    asan=1;
#endif
#if defined(XR_BUILD_UBSAN)
    ubsan=1;
#endif
    CHECK(printf("ALLOCATION_BEGIN {\"schema\":\"allocation-case-faults.v1\",\"mode\":\"%s\","
        "\"index\":%zu,\"workers\":%zu,\"nonce\":\"%s\",\"input_sha256\":\"%s\","
        "\"binary_sha256\":\"%s\",\"config_sha256\":\"%s\",\"asan\":%u,\"ubsan\":%u}\n",
        mode,index,workers,nonce,input,binary,config,asan,ubsan)>0);
    CHECK(fflush(stdout)==0 && !ferror(stdout));
}
static void allocation_worker_case(size_t index) {
    CHECK(index<ALLOCATION_WORKER_CASES);
    if(index<2)function_allocation_phase_case(index!=0);
    else if(index==2)function_producer_allocation_phase_case();
    else if(index<11)invocation_runtime_allocation_phase_case((unsigned)(index-3));
    else if(index<19)invocation_direct_allocation_phase_case((unsigned)(index-11));
    else {
        unsigned group=(unsigned)((index-19)/8),mode=(unsigned)((index-19)%8);
        bool scalar=group>=3;
        if(group%3==0)invocation_capture_allocation_phase_case(mode,scalar);
        else if(group%3==1)invocation_return_capture_allocation_phase_case(mode,scalar);
        else invocation_function_cell_allocation_phase_case(mode,scalar);
    }
}
static unsigned allocation_worker_axes(size_t index) {
    if(index<10 || (index-3)%8!=7)return 0;
    unsigned group=(unsigned)((index-10)/8);CHECK(group<8);
    if(group<2) {
        allocation_phase_case_begin(group?"direct":"runtime",0,0);allocation_stage="resource-axes";
        if(group)invocation_direct_owner_axes();else invocation_runtime_owner_axes();
    } else {
        unsigned family=(group-2)%3;bool scalar=group>=5;
        const char *name=family==0?"capture":family==1?"return_capture":"function_cell";
        allocation_phase_case_begin(name,scalar,0);allocation_stage="resource-axes";
        if(family==0)invocation_capture_owner_axes(scalar);
        else if(family==1)invocation_return_capture_owner_axes(scalar);
        else invocation_function_cell_owner_axes(scalar);
    }
    CHECK(!live && !class_live_bytes);return 1u<<group;
}
static void allocation_worker_complete(size_t expected_cases,unsigned axis_mask,bool resources) {
    CHECK(allocation_phase_cases==expected_cases && !live && !class_live_bytes && fail_at==SIZE_MAX);
    unsigned groups=0;for(unsigned bits=axis_mask;bits;bits>>=1)groups+=bits&1;
    CHECK(printf("ALLOCATION_END {\"cases\":%zu,\"whole\":%zu,\"compiler\":%zu,"
        "\"runtime\":%zu,\"bridge\":%zu,\"axis_groups\":%u,\"axes\":%u,"
        "\"remaining_resources\":%u,\"physical_blocks\":%zu,\"physical_bytes\":%zu}\n",
        allocation_phase_cases,allocation_phase_ordinals,allocation_phase_compiler_ordinals,
        allocation_phase_runtime_ordinals,allocation_phase_bridge_ordinals,axis_mask,groups*11,
        (unsigned)resources,live,class_live_bytes)>0);
    CHECK(fflush(stdout)==0 && !ferror(stdout));
}
static bool allocation_worker_dispatch(int argc,char **argv) {
    if(argc<2)return false;
    bool census=!strcmp(argv[1],"--allocation-census"),shard=!strcmp(argv[1],"--allocation-shard");
    if(!census && !shard)return false;
    CHECK(argc==(census?3:6));
    if(census) {
        allocation_worker_begin("census",0,1,argv[2]);allocation_phase_selection=0;
        allocation_phase_matrix(false);allocation_worker_complete(ALLOCATION_WORKER_CASES,0,false);return true;
    }
    size_t index=allocation_worker_number(argv[2]),workers=allocation_worker_number(argv[3]);
    CHECK(workers && workers<=ALLOCATION_WORKER_CASES && index<workers);
    const char *mode=argv[4];bool resources=false;
    if(!strcmp(mode,"normal"))allocation_phase_selection=0;
    else if(!strcmp(mode,"producer"))allocation_phase_selection=ALLOCATION_PHASE_COMPILER;
    else if(!strcmp(mode,"matrix") || !strcmp(mode,"aggregate"))allocation_phase_selection=7;
    else CHECK(false);
    allocation_worker_begin(mode,index,workers,argv[5]);
    if(!strcmp(mode,"aggregate") && !index) {allocation_remaining_resources();resources=true;}
    unsigned axis_mask=0;size_t cases=0;
    for(size_t current=index;current<ALLOCATION_WORKER_CASES;current+=workers) {
        size_t before=allocation_phase_cases;allocation_worker_case(current);
        CHECK(allocation_phase_cases==before+1 && !live && !class_live_bytes);
        unsigned axes=allocation_worker_axes(current);CHECK(!(axis_mask&axes));axis_mask|=axes;++cases;
    }
    allocation_worker_complete(cases,axis_mask,resources);return true;
}
#endif // XIR_ALLOCATION_CASE_WORKERS_H
