/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "../../xir/product_facts_control_fixture.h"
static void product_facts_print_digest(const char *name,const uint8_t bytes[32]) {
    printf(" %s=",name);
    for(unsigned i=0;i<32;++i)printf("%02x",(unsigned)bytes[i]);
}
static bool product_facts_keep_going(const void *context) {
    size_t *polls=(size_t *)context;++*polls;return false;
}
static void product_facts_active_equal(const XrXirSourceProductRequest *original,
    const XrXirSourceText *source,const XrXirSourceProduct *expected,bool file) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(original->source.context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    size_t polls=0;XrCompileRequestOptions options={.cancel_poll=product_facts_keep_going,.cancel_context=&polls};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceProductRequest request=*original;request.source.session=session;
    XrXirSourceProduct *actual=NULL;
    XrXirStatus status=file?xr_xir_compile_source_product_build(&request,&actual,NULL):
        xr_xir_compile_source_product_build_text(&request,source,&actual,NULL);
    CHECK(status==XR_XIR_OK&&actual&&polls);xr_compile_session_free(session);
    const XrXirSourceProductFacts *a=xr_xir_compile_source_product_facts(actual),
        *b=xr_xir_compile_source_product_facts(expected);
    CHECK(a->entry==b->entry&&a->function_count==b->function_count&&a->module_count==b->module_count);
    CHECK(a->target.architecture==b->target.architecture&&a->target.abi_version==b->target.abi_version);
    CHECK(!memcmp(a->source_digest,b->source_digest,32)&&!memcmp(a->closed_digest,b->closed_digest,32)&&
        !memcmp(a->lowered_layout_digest,b->lowered_layout_digest,32));
    for(unsigned kind=0;kind<2;++kind) {
        XrXirSourceProductPacketView left={0},right={0};
        CHECK(xr_xir_compile_source_product_packet(actual,(XrXirSourceProductPacketKind)kind,&left)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_packet(expected,(XrXirSourceProductPacketKind)kind,&right)==XR_XIR_OK);
        CHECK(left.length==right.length&&!memcmp(left.bytes,right.bytes,left.length));
    }
    const XrXirSourceTests *left=xr_xir_compile_source_product_tests(actual),
        *right=xr_xir_compile_source_product_tests(expected);CHECK(left->count==right->count);
    for(uint32_t i=0;i<left->count;++i) {
        const XrXirSourceTestEntry *x=&left->entries[i],*y=&right->entries[i];
        CHECK(x->function==y->function&&x->role==y->role&&x->timeout_seconds==y->timeout_seconds);
        CHECK(x->name_length==y->name_length&&!memcmp(x->name,y->name,x->name_length+1));
    }
    size_t retired_polls=polls;product_packet_after_program(actual);xr_xir_compile_source_product_free(actual);
    CHECK(polls==retired_polls);
}
static void product_facts_baseline_one(const char *text,size_t length,const char *name,
    const char *entry_path,uint32_t tests_count) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    uint64_t held=sdk_stats(resources).held_bytes;
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    char canonical_entry[4096],canonical_root[4096];
    if(entry_path) {
        XrOsIoPolicy policy=xr_compile_io_policy(resources);
        CHECK(xr_os_io_realpath(&policy,entry_path,canonical_entry,sizeof(canonical_entry))==XR_OS_IO_OK);
        CHECK(xr_os_io_realpath(&policy,XR_PUBLIC_STDLIB "/../tests/unit/xir/test_declarations_owner",
            canonical_root,sizeof(canonical_root))==XR_OS_IO_OK);
        entry_path=canonical_entry;
        authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,canonical_root};
    }
    XrXirCompileContext context={0};
    XrXirSourceProductRequest request=product_packet_request(resources,session,&authority,&context);
    request.source.entry_path=entry_path;
    XrXirSourceText source={NULL,text,length};XrXirSourceProduct *product=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};ProductFactsProbe probe={0};
    CHECK(!product_facts_probe);product_facts_probe=&probe;
    XrXirStatus status=entry_path?
        xr_xir_compile_source_product_build(&request,&product,&diagnostic):
        xr_xir_compile_source_product_build_text(&request,&source,&product,&diagnostic);
    product_facts_probe=NULL;
    if(status!=XR_XIR_OK) {
        XrFrontendRoot root={.message=""};
        if(diagnostic.failure.report)CHECK(xr_frontend_report_root(diagnostic.failure.report,0,&root));
        fprintf(stderr,"facts baseline %s status%u stage%u %d:%d: %s\n",name,
            (unsigned)status,(unsigned)diagnostic.stage,root.primary.line,root.primary.column,root.message);
    }
    CHECK(status==XR_XIR_OK&&product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);CHECK(facts);
    uint8_t digest[32];xr_sha256(probe.preimage,probe.bytes,digest);
    CHECK(!memcmp(digest,facts->lowered_layout_digest,32));
    const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(product);
    CHECK(tests&&tests->count==tests_count);
    if(tests_count==46) {
        size_t long_names=0;
        for(uint32_t i=0;i<tests->count;++i)if(tests->entries[i].name_length==5000)++long_names;
        CHECK(long_names==1);
    }
    printf("Product facts baseline %s sourceBytes=%zu functions=%u tests=%u layoutBytes=%zu prefix4122=%u",
        name,length,facts->function_count,tests->count,probe.bytes,probe.prefix_seen?1u:0u);
    product_facts_print_digest("source",facts->source_digest);
    product_facts_print_digest("closed",facts->closed_digest);
    product_facts_print_digest("layout",facts->lowered_layout_digest);
    if(probe.prefix_seen)product_facts_print_digest("prefix",probe.prefix_digest);
    putchar('\n');
    product_facts_active_equal(&request,&source,product,entry_path!=NULL);
    product_packet_after_program(product);
    for(uint32_t i=0;i<tests->count;++i) {
        const XrXirSourceTestEntry *entry=&tests->entries[i];
        CHECK(entry->name&&entry->name[entry->name_length]==0);
    }
    xr_xir_compile_source_product_free(product);
    CHECK(sdk_stats(resources).held_bytes==held);xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);
}
static void product_facts_baseline(void) {
    char implementation[8000];size_t length=implementation_control_fixture(implementation,sizeof(implementation));
    CHECK(length==7658);
    product_facts_baseline_one(implementation,length,"facts-implementation",NULL,0);
    char flow[4096];FILE *file=fopen(XR_PUBLIC_STDLIB "/../demos/01-basics/control_flow.xr","rb");CHECK(file);
    length=fread(flow,1,sizeof(flow),file);CHECK(!ferror(file)&&feof(file)&&length==2127&&fclose(file)==0);
    product_facts_baseline_one(flow,length,"facts-control-flow",NULL,0);
    product_facts_baseline_one(NULL,0,"facts-original-tests",
        XR_PUBLIC_STDLIB "/../tests/unit/xir/test_declarations_owner/basic.xr",7);
    char directory[9000];length=product_facts_directory_fixture(directory,sizeof(directory));
    product_facts_baseline_one(directory,length,"facts-directory",NULL,46);
    puts("Complete Product FACTS baseline: actual layout framing, directory ownership and packet retirement PASS");
}
static bool product_facts_poll(const void *context) {
    ProductFactsProbe *probe=(ProductFactsProbe *)context;++probe->polls;
    if(probe->disabled||probe->witnesses||probe->mode==2)return false;
    const char *caller=probe->poll_function?probe->poll_function:"";
    bool reached=false;
    if(probe->phase==0)reached=!strcmp(caller,"source_product_layout_digest")&&!probe->bytes;
    else if(probe->phase==1) {
        reached=!strcmp(caller,"source_layout_words")&&probe->prefix_seen&&probe->bytes==4122;
        if(reached) {
            static const uint8_t expected[]={0xe1,0x63,0x2e,0x21,0x6d,0x2d,0x61,0x6c,
                0xc2,0x07,0xc9,0x29,0x00,0x0b,0xa8,0x5a,0x8a,0xc9,0xe9,0x41,0x61,0x2e,
                0x9d,0x72,0xc1,0x3b,0xa8,0xe8,0xe4,0x18,0x33,0x6e};
            CHECK(!memcmp(probe->prefix_digest,expected,32)&&probe->hash->count==4122);
        }
    } else if(probe->phase==2)
        reached=!strcmp(caller,"source_product_layout_digest")&&probe->bytes==11838;
    else if(probe->phase==3)reached=!strcmp(caller,"source_product_tests")&&!probe->directory;
    else if(probe->phase==5) {
        reached=probe->name_destination&&!memcmp(probe->name_destination,probe->name_source,4096);
        if(reached) {
            CHECK(probe->name_bytes==5000);
            for(size_t i=4096;i<probe->name_bytes;++i)CHECK((unsigned char)probe->name_destination[i]==0xa5);
        }
    } else {
        CHECK(probe->phase==4||probe->phase==6);
        if(probe->directory&&!strcmp(caller,"source_product_tests")) {
            const XrXirSourceTestEntry *entries=probe->directory;size_t copied=0;
            while(copied<probe->directory_count&&entries[copied].name) {
                const XrXirSourceTestEntry *a=&entries[copied],*b=&probe->expected_entries[copied];
                CHECK(a->function==b->function&&a->role==b->role&&a->timeout_seconds==b->timeout_seconds);
                CHECK(a->name_length==b->name_length&&!memcmp(a->name,b->name,a->name_length+1));++copied;
            }
            if(probe->phase==4) {
                reached=copied&&copied<probe->directory_count&&entries[copied-1].function<32&&
                    probe->expected_entries[copied].function>=32;
                if(reached)for(size_t i=copied;i<probe->directory_count;++i)
                    CHECK(!entries[i].function&&!entries[i].role&&!entries[i].name);
            } else reached=copied==probe->directory_count;
            if(reached)CHECK(probe->product&&!((const XrXirSourceProduct *)probe->product)->tests.entries);
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
static int product_facts_public_poll(const void *context){return product_facts_poll(context)?1:0;}
static XrXirSourceProduct *product_facts_reference(const char *text,size_t length,const char *name,
    XrCompileResources **resources) {
    *resources=sdk_ledger(&sdk_unlimited);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(*resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};XrXirCompileContext context={0};
    XrXirSourceProductRequest request=product_packet_request(*resources,session,&authority,&context);
    XrXirSourceText source={NULL,text,length};XrXirSourceProduct *product=NULL;
    CHECK(xr_xir_compile_source_product_build_text(&request,&source,&product,NULL)==XR_XIR_OK&&product);
    xr_compile_session_free(session);return product;
}
static void product_facts_borrowed(XrXirSourceProduct *reference,XrCompileResources *resources) {
    uint64_t held=sdk_stats(resources).held_bytes;size_t live=runtime_live,bytes=runtime_bytes;
    for(unsigned malformed=0;malformed<3;++malformed) {
        XrXirSourceProductFacts facts=reference->facts;
        if(malformed==0)++facts.entry;
        else if(malformed==1)++facts.function_count;
        else ++facts.target.abi_version;
        ProductFactsProbe probe={.phase=0,.armed=true,.resources=resources};
        XrCompileRequestOptions options={.cancel_poll=product_facts_poll,.cancel_context=&probe};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        uint8_t digest[32],saved[32];memset(digest,0xa5,32);memcpy(saved,digest,32);
        CHECK(!product_facts_probe);product_facts_probe=&probe;
        CHECK(source_product_layout_digest(reference->lowered,&facts,digest,&control)==XR_XIR_BAD_STRUCTURE);
        product_facts_probe=NULL;CHECK(!probe.polls&&!probe.bytes&&!memcmp(digest,saved,32));
    }
    for(unsigned mode=0;mode<2;++mode) {
        ProductFactsProbe probe={.phase=1,.mode=mode,.armed=true,.resources=resources};
        XrCompileRequestOptions options={.cancel_poll=product_facts_poll,.cancel_context=&probe};
        if(mode){options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=UINT64_C(100000000);}
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        uint8_t digest[32],saved[32];memset(digest,0xa5,32);memcpy(saved,digest,32);
        CHECK(!product_facts_probe);product_facts_probe=&probe;
        XrXirStatus status=source_product_layout_digest(reference->lowered,&reference->facts,digest,&control);
        product_facts_probe=NULL;
        CHECK(status==(mode?XR_XIR_DEADLINE:XR_XIR_CANCELLED)&&probe.witnesses==1&&!memcmp(digest,saved,32));
        CHECK(source_product_layout_digest(reference->lowered,&reference->facts,digest,NULL)==XR_XIR_OK);
        CHECK(!memcmp(digest,reference->facts.lowered_layout_digest,32));
        CHECK(sdk_stats(resources).held_bytes==held&&runtime_live==live&&runtime_bytes==bytes);
    }
}
static void product_facts_cases(const char *selected) {
    static const struct {const char *name;unsigned phase,mode;} cases[]={
        {"layout_entry",0,0},{"layout_prefix",1,0},{"layout_final",2,0},
        {"directory_entry",3,0},{"directory_rows",4,0},{"directory_name",5,0},
        {"directory_publish",6,0},{"layout_deadline",1,1},{"directory_deadline",5,1},
        {"directory_oom",6,2}};
    unsigned choice=0;
    if(selected){while(choice<sizeof(cases)/sizeof(*cases)&&strcmp(selected,cases[choice].name))++choice;CHECK(choice<sizeof(cases)/sizeof(*cases));}
    product_facts_baseline();
    char text[9000];size_t length=implementation_control_fixture(text,sizeof(text));
    XrCompileResources *layout_resources=NULL,*directory_resources=NULL;
    XrXirSourceProduct *layout=product_facts_reference(text,length,"facts-implementation",&layout_resources);
    product_facts_borrowed(layout,layout_resources);
    length=product_facts_directory_fixture(text,sizeof(text));
    XrXirSourceProduct *directory=product_facts_reference(text,length,"facts-directory",&directory_resources);
    const XrXirSourceTests *expected=xr_xir_compile_source_product_tests(directory);CHECK(expected->count==46);
    size_t live=runtime_live,bytes=runtime_bytes;
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        if(selected&&n!=choice)continue;
        bool test_directory=cases[n].phase>=3;
        length=test_directory?product_facts_directory_fixture(text,sizeof(text)):
            implementation_control_fixture(text,sizeof(text));
        ProductFactsProbe probe={.phase=cases[n].phase,.mode=cases[n].mode,.armed=true,
            .directory_count=expected->count,.expected_entries=expected->entries};
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
        options.cancel_poll=product_facts_public_poll;options.cancel_context=&probe;
        if(probe.mode==1){options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=UINT64_C(5000000000);}
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,
            test_directory?"facts-directory":"facts-implementation",text,length}};
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *report=NULL;Calibration calibration={0};
        CHECK(!product_facts_probe);product_facts_probe=&probe;
        XrSourceStatus status=calibration_build(&input,&options,&calibration,&artifact,&failure,&report);
        product_facts_probe=NULL;probe.control=NULL;probe.product=NULL;probe.directory=NULL;probe.hash=NULL;
        XrSourceStatus wanted=probe.mode==1?XR_SOURCE_DEADLINE:probe.mode==2?XR_SOURCE_OUT_OF_MEMORY:XR_SOURCE_CANCELLED;
        fprintf(stderr,"Product FACTS %s status%u stage%u cleanup%u witness%zu layoutBytes%zu allocations%zu nameBytes%zu\n",
            cases[n].name,(unsigned)status,(unsigned)failure.stage,(unsigned)failure.cleanup_status,
            probe.witnesses,probe.bytes,probe.allocations,probe.name_bytes);
        CHECK(status==wanted&&!artifact&&failure.status==wanted&&failure.cleanup_status==XR_SOURCE_OK);
        CHECK(failure.stage==XR_SOURCE_STAGE_COMPILE&&probe.resources);
        CHECK(probe.mode==2?probe.oom_seen:probe.witnesses==1);
        xray_source_diagnostic_free(report);
        CHECK(sdk_stats(probe.resources).held_bytes==probe.held_empty);
        xr_compile_resources_release(probe.resources);probe.resources=NULL;
        CHECK(runtime_live==live&&runtime_bytes==bytes);
        probe.disabled=true;
        if(probe.mode==1){options.deadline_mode=XR_SOURCE_DEADLINE_NONE;options.timeout_ns=0;}
        CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        size_t polls=probe.polls;DeclarationsCopyProbe retired={0};implementation_execute(artifact,text,length,&retired);
        CHECK(probe.polls==polls&&runtime_live==live&&runtime_bytes==bytes);
    }
    xr_xir_compile_source_product_free(directory);xr_compile_resources_release(directory_resources);
    xr_xir_compile_source_product_free(layout);xr_compile_resources_release(layout_resources);
    CHECK(!runtime_live&&!runtime_bytes);
    puts("Product FACTS: actual layout prefix, directory rows/name/publication, expiry/OOM rollback and retired ready42 PASS");
}
