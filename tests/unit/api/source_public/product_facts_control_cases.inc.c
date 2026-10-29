/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static void product_facts_print_digest(const char *name,const uint8_t bytes[32]) {
    printf(" %s=",name);
    for(unsigned i=0;i<32;++i)printf("%02x",(unsigned)bytes[i]);
}
static size_t product_facts_directory_fixture(char *text,size_t capacity) {
    size_t used=0;
#define PF_APPEND(...) do {int count=snprintf(text+used,capacity-used,__VA_ARGS__);CHECK(count>=0&&(size_t)count<capacity-used);used+=(size_t)count;}while(0)
    PF_APPEND("@before_all\nfn prepare(){}\n@before_each\nfn before_case(){}\n");
    for(unsigned n=0;n<40;++n)PF_APPEND("@test\nfn case%u(){assertEqual(1+1,2)}\n",n);
    PF_APPEND("@test(skip)\nfn ");
    for(unsigned n=0;n<5000;++n)PF_APPEND("a");
    PF_APPEND("(){print(\"must not run\")}\n@test(timeout:7)\nfn timed(){}\n"
        "@after_each\nfn after_case(){}\n@after_all\nfn finish(){}\n"
        "export fn answer()->i64{return 42}\nprint(answer())\n");
#undef PF_APPEND
    return used;
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
