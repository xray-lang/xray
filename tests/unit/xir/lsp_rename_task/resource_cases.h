/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_RENAME_RESOURCE_CASES_H
#define LSP_RENAME_RESOURCE_CASES_H
static bool rename_resource_scenario(unsigned scenario){return scenario==0||scenario==13||scenario==14||scenario==15;}
static int rename_task_before(const RenameTaskOptions *options) {
    RenameTaskFixture f;rename_task_fixture(&f,options);rename_task_start(&f);
    size_t before=runtime_attempts;XlspPendingRequest *pending=xlsp_pending_request_find(&f.server.pending_requests,&f.id);
    CHECK(pending&&!pending->source_task_id);
    CHECK(xlsp_source_task_submit(&f.server,&f.id,f.params,rename_task_mode(&f))==XR_XIR_BAD_STRUCTURE);
    CHECK(runtime_attempts==before&&!pending->source_task_id);
    atomic_store(&f.gate.release,true);unsigned completed=0;uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!completed){completed+=(unsigned)xlsp_async_poll(f.async);CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    rename_task_close(&f);
    fprintf(stderr,"RENAME_BEFORE method=%u exact_BAD_STRUCTURE=1 no_compiler_admission=1 physical0/0 JSON0/0 app0/0\n",options->method);
    return 0;
}
static int rename_task_main(int argc,char **argv) {
    CHECK(argc>=8);
    RenameTaskOptions o={.uri=argv[1],.library=argv[3],.closed=argv[4],.root=argv[5],
        .method=(unsigned)strtoul(argv[6],NULL,10),.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    CHECK(o.method<2);RenameTaskMetrics metrics={0};
    if(!strcmp(argv[7],"--before")){CHECK(argc==8);return rename_task_before(&o);}
    CHECK(source_request_mode("textDocument/rename")==23&&source_request_mode("textDocument/prepareRename")==24);
    CHECK(source_request_mode("textDocument/inlayHint")==-1&&source_request_mode("workspace/symbol")==22);
    if(!strcmp(argv[7],"--resource")) {
        CHECK(argc==11);o.source_fault=(size_t)strtoull(argv[8],NULL,10);o.json_fault=(size_t)strtoull(argv[9],NULL,10);
        o.scenario=(unsigned)strtoul(argv[10],NULL,10);CHECK(rename_resource_scenario(o.scenario));return rename_task_once(&o,&metrics);
    }
    if(!strcmp(argv[7],"--chunk")) {
        CHECK(argc==13);bool source=!strcmp(argv[8],"source");CHECK(source||!strcmp(argv[8],"json"));
        size_t begin=(size_t)strtoull(argv[9],NULL,10),count=(size_t)strtoull(argv[10],NULL,10),total=(size_t)strtoull(argv[11],NULL,10);
        CHECK(count&&begin<total&&count<=total-begin);o.scenario=(unsigned)strtoul(argv[12],NULL,10);
        CHECK(rename_resource_scenario(o.scenario));o.report=false;
        for(size_t i=begin;i<begin+count;++i){o.source_fault=source?i:SIZE_MAX;o.json_fault=source?SIZE_MAX:i;CHECK(!rename_task_once(&o,&metrics));}
        fprintf(stderr,"RENAME_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",argv[8],begin,count,total,count);return 0;
    }
    if(!strcmp(argv[7],"--axes")) {
        CHECK(argc==12);o.report=false;o.scenario=(unsigned)strtoul(argv[11],NULL,10);CHECK(rename_resource_scenario(o.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[8],NULL,10),(uint64_t)strtoull(argv[9],NULL,10),(uint64_t)strtoull(argv[10],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits cut=baseline;uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;
            CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);o.limits=&cut;o.budget=delta<0;
            CHECK(!rename_task_once(&o,&metrics));
        }
        fprintf(stderr,"RENAME_AXES method=%u scenario=%u checked=9 physical0/0 JSON0/0 app0/0\n",o.method,o.scenario);return 0;
    }
    CHECK(argc==8);o.scenario=(unsigned)strtoul(argv[7],NULL,10);
    CHECK(o.scenario<=18);
    return rename_task_once(&o,&metrics);
}
#endif // LSP_RENAME_RESOURCE_CASES_H
