/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
typedef struct PacketSpanProbe {
    const unsigned char *source,*destination;
    XrSHA256Context *hash;
    size_t polls;
    bool reject;
} PacketSpanProbe;
static bool packet_span_poll(const void *context) {
    PacketSpanProbe *probe=(PacketSpanProbe *)context;++probe->polls;
    if(!probe->reject)return false;
    if(probe->hash)return probe->hash->count==4096;
    return !probe->destination||!memcmp(probe->destination,probe->source,4096);
}
static void packet_codec_vectors(void) {
    unsigned char source[8197],encoded[8201];
    for(size_t i=0;i<sizeof(source);++i)source[i]=(unsigned char)('A'+i%23);
    XrCompileRequestControl control={0};PacketSpanProbe probe={.reject=true};
    XrCompileRequestOptions options={.cancel_poll=packet_span_poll,.cancel_context=&probe};
    XrSHA256Context hash;xr_sha256_init(&hash);
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_lower_sha256_span(&control,&hash,NULL,0)==XR_COMPILE_RESOURCE_CANCELLED&&hash.count==0);
    CHECK(xir_lower_sha256_span(NULL,&hash,NULL,0)==XR_COMPILE_RESOURCE_OK&&hash.count==0);
    probe=(PacketSpanProbe){.hash=&hash,.reject=true};control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_lower_sha256_span(&control,&hash,source,sizeof(source))==XR_COMPILE_RESOURCE_CANCELLED&&hash.count==4096);
    uint8_t actual[32],expected[32];xr_sha256_final(&hash,actual);xr_sha256(source,4096,expected);
    CHECK(!memcmp(actual,expected,32));
    probe.reject=false;control=(XrCompileRequestControl){0};xr_sha256_init(&hash);
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_lower_sha256_span(&control,&hash,source,sizeof(source))==XR_COMPILE_RESOURCE_OK&&hash.count==sizeof(source));
    xr_sha256_final(&hash,actual);xr_sha256(source,sizeof(source),expected);CHECK(!memcmp(actual,expected,32));
    for(unsigned nominal=0;nominal<2;++nominal) {
        memset(encoded,0xa5,sizeof(encoded));
        probe=(PacketSpanProbe){.source=source,.destination=encoded+4,.reject=true};
        control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        CheckedCursor cursor={.output=encoded,.capacity=sizeof(encoded),.structural=context.limits,
            .remaining=context,.status=XR_XIR_OK,.control=&control};
        if(nominal) {
            XrXirLiteral name={(const char *)source,sizeof(source)};
            XrXirLiteral preserved=checked_nominal_name(&cursor,name);
            CHECK(preserved.bytes==name.bytes&&preserved.length==name.length);
        } else {
            uint32_t length=sizeof(source);
            CHECK(checked_blob(&cursor,(const char *)source,&length)==(const char *)source&&length==sizeof(source));
        }
        CHECK(cursor.status==XR_XIR_CANCELLED&&cursor.position==4&&probe.polls>1);
        CHECK(!memcmp(encoded+4,source,4096));
        for(size_t i=4100;i<sizeof(encoded);++i)CHECK(encoded[i]==0xa5);
    }
    probe=(PacketSpanProbe){0};control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CheckedCursor cursor={.position=4090,.capacity=SIZE_MAX,.structural=context.limits,
        .remaining=context,.status=XR_XIR_OK,.control=&control,.next_poll=4096};
    XrXirConstruction construction={0},*owner=&construction;XrXirModule module={.stage=XR_XIR_CHECKED};
    checked_module(&cursor,&module,&owner,XR_XIR_EVIDENCE_INSTANCE);
    CHECK(cursor.status==XR_XIR_OK&&cursor.position>4096&&cursor.next_poll>cursor.position&&probe.polls==1);
    checked_u32(&cursor,1);CHECK(cursor.status==XR_XIR_OK&&probe.polls==1);
    puts("Private codec vectors: zero SHA, long blob/name prefix rollback, byte-exact SHA retry and nested cursor progress PASS");
}
static bool packet_codec_poll(const void *context) {
    PacketCodecProbe *probe=(PacketCodecProbe *)context;++probe->polls;
    if(probe->disabled||probe->witnesses)return false;
    if(probe->phase==0 && probe->packet && !probe->wanted &&
        !memcmp(probe->packet,probe->expected[1],32) &&
        !memcmp(probe->packet+64,probe->expected[1]+64,4096-64)) {
        CHECK(probe->packet[probe->lengths[1]-1]!=(unsigned char)probe->expected[1][probe->lengths[1]-1]);
        probe->wanted=true;
    }
    if(!probe->wanted)return false;
    if(probe->phase) {
        uint8_t actual[32],expected[32];XrSHA256Context hash;
        xr_sha256_final(&probe->prefix_hash,actual);xr_sha256_init(&hash);
        if(probe->phase==1) {
            CHECK(probe->hash_bytes==4128);
            xr_sha256_update(&hash,probe->expected[1],32);
            xr_sha256_update(&hash,probe->expected[1]+64,4096);
        } else {
            CHECK(probe->hash_bytes==4096);
            xr_sha256_update(&hash,probe->expected[probe->phase==3?0:1],4096);
        }
        xr_sha256_final(&hash,expected);CHECK(!memcmp(actual,expected,32));
    }
    ++probe->witnesses;
    if(!probe->mode)return true;
    CHECK(probe->mode==1&&probe->control&&probe->control->has_deadline);
    uint64_t now=xr_time_monotonic_ns();CHECK(now);
    while(now<probe->control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();CHECK(now);}
    return false;
}
static int packet_codec_public_poll(const void *context) {return packet_codec_poll(context)?1:0;}
static void packet_codec_input_preserved(const PacketCodecProbe *baseline) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    uint64_t held=sdk_stats(resources).held_bytes;size_t live=runtime_live,bytes=runtime_bytes;
    XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_checked_read(&context,baseline->expected[1],baseline->lengths[1],&closed,NULL)==XR_XIR_OK&&closed);
    PacketCodecProbe probe=*baseline;probe.in_lower=true;probe.resources=resources;
    XrCompileRequestOptions options={.cancel_poll=packet_codec_poll,.cancel_context=&probe};
    XrCompileRequestControl control={0};probe.control=&control;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    uint64_t input_held=sdk_stats(resources).held_bytes;
    size_t input_live=runtime_live,input_bytes=runtime_bytes;
    packet_codec_probe=&probe;
    XrXirStatus status=checked_encode(closed,&packet,&diagnostic,&control);
    packet_codec_probe=NULL;
    CHECK(status==XR_XIR_CANCELLED&&diagnostic.status==status&&!packet.bytes&&!packet.length);
    CHECK(probe.allocations==1&&probe.witnesses==1&&control.status==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(sdk_stats(resources).held_bytes==input_held&&runtime_live==input_live&&runtime_bytes==input_bytes);
    CHECK(checked_encode(closed,&packet,NULL,NULL)==XR_XIR_OK);
    CHECK(packet.length==baseline->lengths[1]&&!memcmp(packet.bytes,baseline->expected[1],packet.length));
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(closed);
    CHECK(sdk_stats(resources).held_bytes==held&&runtime_live==live&&runtime_bytes==bytes);
    xr_compile_resources_release(resources);
    puts("Borrowed Checked encode cancellation: original complete input re-encodes byte-exactly and retires without leaks PASS");
}
static void packet_codec_controlled_equal(XrXirSourceProductRequest *request,
    const XrXirSourceText *input,const XrXirSourceProduct *expected) {
    XrCompileResources *resources=request->source.context->resources;
    uint64_t held=sdk_stats(resources).held_bytes;size_t live=runtime_live,bytes=runtime_bytes;
    PacketCodecProbe quiet={.disabled=true};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=packet_codec_poll,.cancel_context=&quiet};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);request->source.session=session;
    XrXirSourceProduct *actual=NULL;
    CHECK(xr_xir_compile_source_product_build_text(request,input,&actual,NULL)==XR_XIR_OK&&actual);
    xr_compile_session_free(session);request->source.session=NULL;
    const XrXirSourceProductFacts *a=xr_xir_compile_source_product_facts(actual);
    const XrXirSourceProductFacts *b=xr_xir_compile_source_product_facts(expected);CHECK(a&&b);
    CHECK(a->entry==b->entry&&a->function_count==b->function_count&&a->module_count==b->module_count);
    CHECK(a->target.architecture==b->target.architecture&&a->target.abi_version==b->target.abi_version);
    CHECK(!memcmp(a->source_digest,b->source_digest,32)&&!memcmp(a->closed_digest,b->closed_digest,32)&&
        !memcmp(a->lowered_layout_digest,b->lowered_layout_digest,32));
    for(unsigned i=0;i<2;++i) {
        XrXirSourceProductPacketView left={0},right={0};
        CHECK(xr_xir_compile_source_product_packet(actual,(XrXirSourceProductPacketKind)i,&left)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_packet(expected,(XrXirSourceProductPacketKind)i,&right)==XR_XIR_OK);
        CHECK(left.length==right.length&&!memcmp(left.bytes,right.bytes,left.length));
    }
    CHECK(quiet.polls>0&&control.status==XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_source_product_free(actual);
    CHECK(sdk_stats(resources).held_bytes==held&&runtime_live==live&&runtime_bytes==bytes);
    puts("Same-authority controlled/default complete SOURCE/Closed packets and all Product facts byte-exact PASS");
}
static void packet_codec_cases(const char *selected) {
    packet_codec_vectors();
    static const struct {const char *name;unsigned phase,mode;} cases[]={
        {"encode",0,0},{"packet_hash",1,0},{"identity_hash",2,0},{"facts_hash",3,0},
        {"deadline_hash",2,1},{"deadline_facts",3,1}};
    unsigned choice=0;
    if(selected){while(choice<sizeof(cases)/sizeof(*cases)&&strcmp(selected,cases[choice].name))++choice;CHECK(choice<sizeof(cases)/sizeof(*cases));}
    char text[8000];size_t length=implementation_control_fixture(text,sizeof(text));CHECK(length==7658);
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);uint64_t held=sdk_stats(resources).held_bytes;
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"packet-codec",NULL};
    XrXirCompileContext context={0};XrXirSourceProductRequest request=product_packet_request(resources,session,&authority,&context);
    XrXirSourceText supplied={NULL,text,length};XrXirSourceProduct *product=NULL;
    uint64_t start=xr_time_monotonic_ns();CHECK(start);
    CHECK(xr_xir_compile_source_product_build_text(&request,&supplied,&product,NULL)==XR_XIR_OK&&product);
    uint64_t baseline_ns=xr_time_monotonic_ns()-start;
    xr_compile_session_free(session);session=NULL;
    PacketCodecProbe baseline={0};
    for(unsigned i=0;i<2;++i) {
        XrXirSourceProductPacketView packet={0};
        CHECK(xr_xir_compile_source_product_packet(product,(XrXirSourceProductPacketKind)i,&packet)==XR_XIR_OK&&packet.length>8192);
        baseline.expected[i]=packet.bytes;baseline.lengths[i]=packet.length;
    }
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);CHECK(facts);
    uint8_t digest[32];
    for(unsigned i=0;i<2;++i) {
        xr_sha256(baseline.expected[i],baseline.lengths[i],digest);
        CHECK(!memcmp(digest,i?facts->closed_digest:facts->source_digest,32));
    }
    fprintf(stderr,"packet codec complete7658 baselineNs%llu Source%zu Closed%zu whole identities PASS\n",
        (unsigned long long)baseline_ns,baseline.lengths[0],baseline.lengths[1]);
    if(!selected||!strcmp(selected,"packet_hash"))packet_codec_controlled_equal(&request,&supplied,product);
    packet_codec_input_preserved(&baseline);
    size_t initial_live=runtime_live,initial_bytes=runtime_bytes;
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        if(selected&&n!=choice)continue;
        PacketCodecProbe probe=baseline;probe.phase=cases[n].phase;probe.mode=cases[n].mode;
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
        options.cancel_poll=packet_codec_public_poll;options.cancel_context=&probe;
        if(probe.mode){CHECK(baseline_ns<UINT64_MAX-UINT64_C(5000000000));options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=baseline_ns+UINT64_C(5000000000);}
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"packet-codec",text,length}};
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *report=NULL;Calibration calibration={0};
        CHECK(!packet_codec_probe);packet_codec_probe=&probe;start=xr_time_monotonic_ns();CHECK(start);
        XrSourceStatus status=calibration_build(&input,&options,&calibration,&artifact,&failure,&report);
        uint64_t elapsed=xr_time_monotonic_ns()-start;packet_codec_probe=NULL;
        probe.control=NULL;probe.packet=NULL;
        fprintf(stderr,"packet codec %s status%u stage%u cleanup%u witness%zu allocations%zu hashPrefix%zu elapsedNs%llu timeoutNs%llu\n",
            cases[n].name,(unsigned)status,(unsigned)failure.stage,(unsigned)failure.cleanup_status,probe.witnesses,
            probe.allocations,probe.hash_bytes,(unsigned long long)elapsed,(unsigned long long)options.timeout_ns);
        XrSourceStatus expected=probe.mode?XR_SOURCE_DEADLINE:XR_SOURCE_CANCELLED;
        CHECK(status==expected&&!artifact&&failure.status==expected&&failure.cleanup_status==XR_SOURCE_OK);
        CHECK(failure.stage==XR_SOURCE_STAGE_COMPILE&&probe.witnesses==1&&probe.resources);
        xray_source_diagnostic_free(report);report=NULL;
        CHECK(sdk_stats(probe.resources).held_bytes==probe.held_empty);
        xr_compile_resources_release(probe.resources);probe.resources=NULL;
        CHECK(runtime_live==initial_live&&runtime_bytes==initial_bytes);
        /* Cancellation retries the same options. Expiry is followed by a new
         * default request; a finite fresh deadline does not promise success. */
        probe.disabled=true;
        if(probe.mode){options.deadline_mode=XR_SOURCE_DEADLINE_NONE;options.timeout_ns=0;}
        CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        CHECK(failure.status==XR_SOURCE_OK&&failure.cleanup_status==XR_SOURCE_OK);
        size_t polls=probe.polls;DeclarationsCopyProbe retired={0};
        implementation_execute(artifact,text,length,&retired);
        CHECK(probe.polls==polls&&runtime_live==initial_live&&runtime_bytes==initial_bytes);
        length=implementation_control_fixture(text,sizeof(text));CHECK(length==7658);
    }
    product_packet_after_program(product);xr_xir_compile_source_product_free(product);
    CHECK(sdk_stats(resources).held_bytes==held);xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);
    puts("Complete7658 Source: actual encoding/SHA prefixes, cancellation/deadline, rollback, fresh ready42/output42LF and retired consumers PASS");
}
