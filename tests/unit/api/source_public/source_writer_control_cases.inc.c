/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static bool source_writer_poll(const void *context) {
    SourceWriterProbe *probe=(SourceWriterProbe *)context;++probe->polls;
    if(probe->disabled||probe->witnesses||probe->mode==2)return false;
    bool reached=false;
    if(probe->phase==0)reached=probe->active==probe->selected&&probe->wanted&&probe->measure_bytes>=4096;
    else if(probe->phase==1) {
        reached=probe->active==probe->selected&&probe->packet&&
            !memcmp(probe->packet,probe->expected,32)&&
            !memcmp(probe->packet+64,probe->expected+64,4096-64);
        if(reached)CHECK(probe->packet[probe->length-1]!=probe->expected[probe->length-1]);
    } else if(probe->phase==2) {
        reached=probe->active==probe->selected&&probe->wanted;
        if(reached) {
            CHECK(probe->hash_bytes==4128);
            uint8_t actual[32],expected[32];XrSHA256Context hash;
            xr_sha256_final(&probe->prefix_hash,actual);xr_sha256_init(&hash);
            xr_sha256_update(&hash,probe->expected,32);
            xr_sha256_update(&hash,probe->expected+64,4096);
            xr_sha256_final(&hash,expected);CHECK(!memcmp(actual,expected,32));
        }
    } else if(probe->phase==3) {
        reached=probe->writes==2&&!probe->active&&probe->wanted&&probe->compare_bytes>=4096&&
            probe->poll_function&&!strcmp(probe->poll_function,"source_product_compare");
    } else if(probe->phase==5) {
        reached=probe->active==probe->selected&&probe->poll_function&&
            !strcmp(probe->poll_function,"source_writer_actual_write");
    } else {
        CHECK(probe->phase==4||probe->phase==6);
        if(probe->debug_table&&probe->poll_function) {
            CHECK(probe->debug_count);
            const XrXirSourceDebugFunction *rows=probe->debug_table;
            if(probe->phase==4)
                reached=!strcmp(probe->poll_function,"source_product_build")&&
                    rows[probe->debug_count-1].function==probe->debug_count-1;
            else
                reached=!strcmp(probe->poll_function,"source_product_debug")&&probe->debug_count>32&&
                    rows[31].function==31&&rows[32].function==0;
        }
    }
    if(!reached)return false;
    ++probe->witnesses;
    if(!probe->mode)return true;
    CHECK(probe->mode==1&&probe->control&&probe->control->has_deadline);
    uint64_t now=xr_time_monotonic_ns();CHECK(now);
    while(now<probe->control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();CHECK(now);}
    return false;
}
static int source_writer_public_poll(const void *context){return source_writer_poll(context)?1:0;}

static void source_writer_borrowed(const SourceWriterProbe *baseline) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirArtifact *input=NULL;
    CHECK(xr_xir_compile_checked_read(&context,baseline->expected,baseline->length,&input,NULL)==XR_XIR_OK&&input);
    uint64_t held=sdk_stats(resources).held_bytes;size_t live=runtime_live,bytes=runtime_bytes;
    SourceWriterProbe probe=*baseline;probe.selected=1;probe.phase=1;probe.direct=true;
    XrCompileRequestOptions options={.cancel_poll=source_writer_poll,.cancel_context=&probe};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    CHECK(!source_writer_probe);source_writer_probe=&probe;
    XrXirStatus status=xir_checked_write(input,&packet,&diagnostic,&control);
    source_writer_probe=NULL;
    CHECK(status==XR_XIR_CANCELLED&&diagnostic.status==status&&!packet.bytes&&!packet.length&&probe.witnesses==1);
    CHECK(sdk_stats(resources).held_bytes==held&&runtime_live==live&&runtime_bytes==bytes);
    CHECK(xr_xir_compile_checked_write(input,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==baseline->length&&!memcmp(packet.bytes,baseline->expected,packet.length));
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(input);
    xr_compile_resources_release(probe.resources);xr_compile_resources_release(resources);
    puts("Full SOURCE writer borrowed input survives cancelled encoding and independent byte-exact re-admission PASS");
}

static void source_writer_cases(const char *selected) {
    static const struct {const char *name;unsigned write,phase,mode;} cases[]={
        {"source_entry",1,5,0},{"debug_entry",2,5,0},
        {"source_size",1,0,0},{"source_encode",1,1,0},{"source_hash",1,2,0},
        {"debug_size",2,0,0},{"debug_encode",2,1,0},{"debug_hash",2,2,0},
        {"debug_compare",0,3,0},{"debug_rows",0,6,0},{"publish",0,4,0},
        {"source_deadline",1,2,1},{"debug_deadline",2,2,1},
        {"source_oom",1,1,2},{"debug_oom",2,1,2}};
    unsigned choice=0;
    if(selected){while(choice<sizeof(cases)/sizeof(*cases)&&strcmp(selected,cases[choice].name))++choice;CHECK(choice<sizeof(cases)/sizeof(*cases));}
    char text[8000];size_t length=implementation_control_fixture(text,sizeof(text));CHECK(length==7658);
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    uint64_t initial_held=sdk_stats(resources).held_bytes;
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-writer",NULL};
    XrXirCompileContext context={0};XrXirSourceProductRequest request=product_packet_request(resources,session,&authority,&context);
    XrXirSourceText supplied={NULL,text,length};XrXirSourceProduct *product=NULL;
    uint64_t start=xr_time_monotonic_ns();CHECK(start);
    CHECK(xr_xir_compile_source_product_build_text(&request,&supplied,&product,NULL)==XR_XIR_OK&&product);
    uint64_t baseline_ns=xr_time_monotonic_ns()-start;
    xr_compile_session_free(session);
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&packet)==XR_XIR_OK&&packet.length>8192);
    SourceWriterProbe baseline={.expected=packet.bytes,.length=packet.length};
    source_writer_borrowed(&baseline);
    size_t initial_live=runtime_live,initial_bytes=runtime_bytes;
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        if(selected&&n!=choice)continue;
        SourceWriterProbe probe=baseline;probe.selected=cases[n].write;probe.phase=cases[n].phase;probe.mode=cases[n].mode;
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
        options.cancel_poll=source_writer_public_poll;options.cancel_context=&probe;
        if(probe.mode==1){CHECK(baseline_ns<UINT64_MAX-UINT64_C(5000000000));options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=baseline_ns+UINT64_C(5000000000);}
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"source-writer",text,length}};
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *report=NULL;Calibration calibration={0};
        CHECK(!source_writer_probe);source_writer_probe=&probe;
        XrSourceStatus status=calibration_build(&input,&options,&calibration,&artifact,&failure,&report);
        source_writer_probe=NULL;probe.control=NULL;probe.packet=NULL;probe.debug_table=NULL;
        fprintf(stderr,"Source writer %s status%u stage%u cleanup%u writes%zu witness%zu allocations%zu measure%zu compare%zu hashPrefix%zu\n",
            cases[n].name,(unsigned)status,(unsigned)failure.stage,(unsigned)failure.cleanup_status,
            probe.writes,probe.witnesses,probe.allocations,probe.measure_bytes,probe.compare_bytes,probe.hash_bytes);
        XrSourceStatus expected=probe.mode==1?XR_SOURCE_DEADLINE:probe.mode==2?XR_SOURCE_OUT_OF_MEMORY:XR_SOURCE_CANCELLED;
        CHECK(status==expected&&!artifact&&failure.status==expected&&failure.cleanup_status==XR_SOURCE_OK);
        CHECK(failure.stage==XR_SOURCE_STAGE_COMPILE&&probe.resources);
        CHECK(probe.mode==2?probe.oom_seen:probe.witnesses==1);
        xray_source_diagnostic_free(report);
        CHECK(sdk_stats(probe.resources).held_bytes==probe.held_empty);
        xr_compile_resources_release(probe.resources);probe.resources=NULL;
        CHECK(runtime_live==initial_live&&runtime_bytes==initial_bytes);
        probe.disabled=true;
        if(probe.mode==1){options.deadline_mode=XR_SOURCE_DEADLINE_NONE;options.timeout_ns=0;}
        CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        size_t polls=probe.polls;DeclarationsCopyProbe retired={0};
        implementation_execute(artifact,text,length,&retired);
        CHECK(probe.polls==polls&&runtime_live==initial_live&&runtime_bytes==initial_bytes);
        length=implementation_control_fixture(text,sizeof(text));CHECK(length==7658);
    }
    product_packet_after_program(product);xr_xir_compile_source_product_free(product);
    CHECK(sdk_stats(resources).held_bytes==initial_held);xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);
    puts("Complete7658 SOURCE/debug writers: real encoding/SHA/comparison/publication interruption, OOM rollback and retired ready42 PASS");
}
