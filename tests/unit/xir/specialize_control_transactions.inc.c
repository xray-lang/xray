/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static XrXirStatus specialize_receipt(void *opaque,const XrXirSpecializationMapView *view) {
    size_t *receipts=opaque;CHECK(view&&view->source&&view->closed&&view->function_count);
    CHECK(view->source!=view->closed);++*receipts;return XR_XIR_OK;
}
static void specialize_source_transactions(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char text[8000];size_t bytes=implementation_control_fixture(text,sizeof(text));XrXirArtifact *source=NULL;
    CHECK(specialize_source(&owner.context,text,bytes,NULL,&source)==XR_XIR_OK);
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *closed=NULL;size_t receipts=0;
    XrXirSpecializationReceiver receiver={&receipts,specialize_receipt};
    specialize_begin(&probe,&control,owner.context.resources,"spec_run",3);
    CHECK(xr_xir_compile_specialize_receive(source,&receiver,&closed,NULL,&control)==XR_XIR_OK);
    size_t final_poll=probe.phase_hits;CHECK(final_poll&&receipts==1);specialize_end(&probe,XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;CHECK(library_compile_stats(&owner.context).held_bytes==held);
    for(unsigned mode=0;mode<3;++mode) {
        receipts=0;specialize_begin(&probe,&control,owner.context.resources,"spec_run",mode);probe.cut=final_poll;
        probe.artifact_slot=(const XrXirArtifact *const *)&closed;
        XrXirStatus status=xr_xir_compile_specialize_receive(source,&receiver,&closed,NULL,&control);
        specialize_end(&probe,status);CHECK(!closed&&receipts==1&&library_compile_stats(&owner.context).held_bytes==held);
    }
    specialize_begin(&probe,&control,owner.context.resources,NULL,3);
    CHECK(xr_xir_compile_specialize_receive(source,&receiver,&closed,NULL,&control)==XR_XIR_OK);specialize_end(&probe,XR_XIR_OK);
    xr_xir_compile_artifact_free(source);memset(text,'!',bytes);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    /* This is the unchanged complete scalar instance Source from the current fixture. */
    static const char scalar[]="fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    for(unsigned mode=0;mode<3;++mode) {
        XrXirArtifact *result=NULL;held=library_compile_stats(&owner.context).held_bytes;
        specialize_begin(&probe,&control,owner.context.resources,"xir_comptime_key_order",mode);
        probe.artifact_slot=(const XrXirArtifact *const *)&result;
        XrXirStatus status=specialize_source(&owner.context,scalar,sizeof(scalar)-1,&control,&result);
        specialize_end(&probe,status);CHECK(!result&&library_compile_stats(&owner.context).held_bytes==held);
    }
    source=NULL;CHECK(specialize_source(&owner.context,scalar,sizeof(scalar)-1,NULL,&source)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);library_compile_owner_drop(&owner);
    puts("complete7658 receiver after-callback final publication + original scalar Source key-order controls/recovery PASS");
}
static XrXirStatus specialize_pending_source(const XrXirCompileContext *context,const char *text,size_t bytes,XrXirArtifact **output) {

    XrXirArtifact *source=NULL;XrXirConstruction *construction=NULL;
    XrXirStatus status=specialize_source(context,text,bytes,NULL,&source);
    if(status==XR_XIR_OK) {
        XrXirComptimeTable table=source->construction->comptime;CHECK(table.binding_count);
        if(!--table.binding_count)table.bindings=NULL;
        XrXirConstructionSpec specification={source->construction->rows,source->construction->count,
            source->construction->materializations,source->construction->materialization_count,&table};
        status=xr_xir_compile_construction_new_v4(context,source->module.types,&specification,
            &source->construction->slots,&construction,NULL);
        if(status==XR_XIR_OK){XrXirModule built=source->module;built.stage=XR_XIR_BUILT;
            status=xr_xir_compile_check_v2(context,&built,construction,output,NULL);}
    }
    xr_xir_compile_construction_free(construction);xr_xir_compile_artifact_free(source);return status;
}
static XrXirStatus specialize_pending_definition(const XrXirCompileContext *context,XrXirArtifact **output) {
    static const char scalar[]="fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    return specialize_pending_source(context,scalar,sizeof(scalar)-1,output);
}
static bool specialize_pending_view_empty(const XirComptimePendingView *view) {
    return !view->definition&&!view->types&&!view->arguments&&!view->argument_count&&
        !view->site.caller&&!view->site.instruction&&!view->site.helper;
}
static void specialize_pending_transactions(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *source=NULL;CHECK(specialize_pending_definition(&owner.context,&source)==XR_XIR_OK);
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XirComptimePending *pending=NULL;
        specialize_begin(&probe,&control,owner.context.resources,"xr_xir_compile_comptime_pending",mode);
        XrXirStatus status=xr_xir_compile_comptime_pending(source,&pending,NULL,&control);
        specialize_end(&probe,status);CHECK(!pending&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(source,&pending,NULL,NULL)==XR_XIR_OK&&pending);
    xr_xir_compile_artifact_free(source);source=NULL;held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XirComptimePendingView view={0};
        specialize_begin(&probe,&control,owner.context.resources,"xr_xir_compile_comptime_pending_view",mode);
        XrXirStatus status=xr_xir_compile_comptime_pending_view(pending,&view,NULL,&control);
        specialize_end(&probe,status);CHECK(specialize_pending_view_empty(&view)&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XirComptimePendingView view={0};CHECK(xr_xir_compile_comptime_pending_view(pending,&view,NULL,NULL)==XR_XIR_OK);
    unsigned char saved=pending->packet.bytes[pending->packet.length-1];pending->packet.bytes[pending->packet.length-1]^=1;
    view=(XirComptimePendingView){0};CHECK(xr_xir_compile_comptime_pending_view(pending,&view,NULL,NULL)==XR_XIR_BAD_STRUCTURE&&specialize_pending_view_empty(&view));
    pending->packet.bytes[pending->packet.length-1]=saved;
    XrXirArtifact *evaluation=NULL;CHECK(xr_xir_compile_comptime_pending_specialize(pending,&evaluation,NULL,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xir_evaluation_write(evaluation,&packet,NULL,NULL)==XR_XIR_OK);
    CHECK(packet.length>76&&!memcmp(packet.bytes,"XRCEV\0\0\0",8));
    held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *received=NULL;
        specialize_begin(&probe,&control,owner.context.resources,"checked_read_mode",mode);probe.cut=2;
        probe.artifact_slot=(const XrXirArtifact *const *)&received;
        XrXirStatus status=xir_evaluation_read(&owner.context,packet.bytes,packet.length,&received,NULL,&control);
        specialize_end(&probe,status);CHECK(!received&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XrXirArtifact *received=NULL;CHECK(xir_evaluation_read(&owner.context,packet.bytes,packet.length,&received,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(evaluation);xr_xir_compile_comptime_pending_free(pending);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    library_compile_owner_drop(&owner);puts("owned pending creation/view/tamper + evaluation decoder final controls and producer retirement PASS");
}
static XrXirArtifact *specialize_take_donor(const XrXirCompileContext *context) {
    static const char text[]="fn identity<T>(value:T)->T{return value}\nexport fn run()->i64{return identity<i64>(41)}\nprint(run())\n";
    XrXirArtifact *source=NULL,*closed=NULL;CHECK(specialize_source(context,text,sizeof(text)-1,NULL,&source)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(source,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(source);return closed;
}
static void specialize_take_transactions(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    for(unsigned mode=0;mode<3;++mode)for(unsigned accepted=0;accepted<2;++accepted) {
        XrXirArtifact *donor=specialize_take_donor(&owner.context),*received=NULL;XrXirEffects *effects=NULL;
        CHECK(xr_xir_compile_artifact_verify_owned_effects(donor->module.provenance->source,&effects,NULL)==XR_XIR_OK);
        XrXirModule module=donor->module;XrXirProvenance *owned=(XrXirProvenance *)donor->module.provenance;donor->module.provenance=NULL;
        XrXirProvenance *identity=owned;SpecializeControlProbe probe;XrCompileRequestControl control;
        XirProvenanceReceiving receiving={donor->construction,effects};
        specialize_begin(&probe,&control,owner.context.resources,
            accepted?"lower_control_poll":"xr_xir_compile_recheck_provenance_take",mode);
        probe.artifact_slot=(const XrXirArtifact *const *)&received;
        XrXirStatus status=xr_xir_compile_recheck_provenance_take(&owner.context,&module,&receiving,&owned,&received,NULL,&control);
        specialize_end(&probe,status);CHECK(!received&&(accepted?!owned:owned==identity));
        if(owned){donor->module.provenance=owned;owned=NULL;}
        xr_xir_compile_artifact_free(donor);xr_xir_compile_effects_free(effects);
        CHECK(library_compile_stats(&owner.context).held_bytes==owner.baseline.held_bytes);
    }
    XrXirArtifact *donor=specialize_take_donor(&owner.context),*received=NULL;XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_artifact_verify_owned_effects(donor->module.provenance->source,&effects,NULL)==XR_XIR_OK);
    XrXirModule module=donor->module;XrXirProvenance *owned=(XrXirProvenance *)donor->module.provenance;donor->module.provenance=NULL;
    XirProvenanceReceiving receiving={donor->construction,effects};SpecializeControlProbe probe;XrCompileRequestControl control;
    specialize_begin(&probe,&control,owner.context.resources,NULL,3);
    CHECK(xr_xir_compile_recheck_provenance_take(&owner.context,&module,&receiving,&owned,(XrXirArtifact **)(void *)&owned,NULL,&control)==XR_XIR_BAD_STRUCTURE);
    CHECK(owned&&probe.polls==0);specialize_end(&probe,XR_XIR_OK);
    source_program_compile_fail_at=source_program_compile_attempts;source_program_compile_injected=false;
    CHECK(xr_xir_compile_recheck_provenance_take(&owner.context,&module,&receiving,&owned,&received,NULL,NULL)==XR_XIR_OUT_OF_MEMORY);
    source_program_compile_fail_at=SIZE_MAX;CHECK(source_program_compile_injected&&!owned&&!received);
    xr_xir_compile_artifact_free(donor);xr_xir_compile_effects_free(effects);
    CHECK(library_compile_stats(&owner.context).held_bytes==owner.baseline.held_bytes);
    donor=specialize_take_donor(&owner.context);CHECK(xr_xir_compile_artifact_verify(donor,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(donor);
    library_compile_owner_drop(&owner);puts("provenance take entry-preserve/accepted-consume, alias before callback, physicalOOM and fresh recovery PASS");
}
/* A separate complete long-literal Source exercises both private packet purposes. */
static void specialize_large_evaluation_controls(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char text[13000];const char *head="fn make<T>()->string{return comptime {const v:Array<T>=[];len(v)==0 ? \"";
    const char *tail="\" : \"bad\"}}\nexport fn run()->i64{const a=make<i8>();const b=make<string>();return len(a)+len(b)}\n";
    size_t used=strlen(head);memcpy(text,head,used);memset(text+used,'x',12300);used+=12300;
    memcpy(text+used,tail,strlen(tail));used+=strlen(tail);
    XrXirArtifact *definition=NULL;CHECK(specialize_pending_source(&owner.context,text,used,&definition)==XR_XIR_OK);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(definition,&pending,NULL,NULL)==XR_XIR_OK&&pending);
    xr_xir_compile_artifact_free(definition);memset(text,'!',used);
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XirComptimePendingView view={0};
        specialize_begin(&probe,&control,owner.context.resources,"compare-prefix",mode);
        XrXirStatus status=xr_xir_compile_comptime_pending_view(pending,&view,NULL,&control);
        specialize_end(&probe,status);CHECK(specialize_pending_view_empty(&view)&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XrXirArtifact *evaluation=NULL;CHECK(xr_xir_compile_comptime_pending_specialize(pending,&evaluation,NULL,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xir_evaluation_write(evaluation,&packet,NULL,NULL)==XR_XIR_OK);
    held=library_compile_stats(&owner.context).held_bytes;
    static const char *const phases[]={"zero-prefix","copy-prefix","sha-prefix"};
    for(unsigned mode=0;mode<3;++mode)for(size_t phase=0;phase<sizeof(phases)/sizeof(*phases);++phase) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *received=NULL;
        specialize_begin(&probe,&control,owner.context.resources,phases[phase],mode);
        probe.artifact_slot=(const XrXirArtifact *const *)&received;
        XrXirStatus status=xir_evaluation_read(&owner.context,packet.bytes,packet.length,&received,NULL,&control);
        specialize_end(&probe,status);CHECK(!received&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XrXirArtifact *received=NULL;CHECK(xir_evaluation_read(&owner.context,packet.bytes,packet.length,&received,NULL,NULL)==XR_XIR_OK);
    XrXirCheckedPacket same={0};CHECK(xir_evaluation_write(received,&same,NULL,NULL)==XR_XIR_OK);
    CHECK(packet.length==same.length&&!memcmp(packet.bytes,same.bytes,packet.length));
    xr_xir_compile_checked_packet_free(&same);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(evaluation);xr_xir_compile_comptime_pending_free(pending);
    uint32_t functions[70];for(size_t i=0;i<70;++i)functions[i]=received->module.provenance->evaluation.callee;
    XirCtfeInstanceBatchRequest request={.checked=received,.functions=functions,.count=70,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirCtfeInstanceBatch *batch=NULL;CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&batch)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(received);held=library_compile_stats(&owner.context).held_bytes;
    XirCtfeInstanceView views[70]={0},empty[70]={0};SpecializeControlProbe probe;XrCompileRequestControl control;
    specialize_begin(&probe,&control,owner.context.resources,"xr_xir_compile_ctfe_instance_batch_views",3);
    CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,70,&control)==XR_XIR_CTFE_OK);
    size_t final_poll=probe.phase_hits;CHECK(final_poll>=3);specialize_end(&probe,XR_XIR_OK);memset(views,0,sizeof(views));
    for(unsigned mode=0;mode<3;++mode)for(unsigned point=0;point<3;++point) {
        specialize_begin(&probe,&control,owner.context.resources,point==1?"compare-prefix":"xr_xir_compile_ctfe_instance_batch_views",mode);
        if(point==2)probe.cut=final_poll;
        XrXirCtfeStatus status=xr_xir_compile_ctfe_instance_batch_views(batch,views,70,&control);
        CHECK(status==(mode==0?XR_XIR_CTFE_CANCELLED:mode==1?XR_XIR_CTFE_DEADLINE:XR_XIR_CTFE_CLOCK_FAILURE));
        specialize_end(&probe,mode==0?XR_XIR_CANCELLED:mode==1?XR_XIR_DEADLINE:XR_XIR_CLOCK_FAILURE);
        CHECK(!memcmp(views,empty,sizeof(views))&&library_compile_stats(&owner.context).held_bytes==held);
    }
    specialize_begin(&probe,&control,owner.context.resources,NULL,3);views[0].instance_function=1;
    CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,70,&control)==XR_XIR_CTFE_BAD_ARGUMENT&&probe.polls==0);
    specialize_end(&probe,XR_XIR_OK);memset(views,0,sizeof(views));
    CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,70,NULL)==XR_XIR_CTFE_OK);
    XrXirValue kept={0};CHECK(xr_xir_value_copy(views[69].value,&kept)==XR_XIR_VALUE_OK);
    xr_xir_compile_ctfe_instance_batch_free(batch);
    xr_compile_resources_release(owner.context.resources);owner.context.resources=NULL;
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&kept,&bytes,&length)&&length==12300);
    for(size_t i=0;i<length;++i)CHECK(bytes[i]=='x');xr_xir_value_drop(&kept);
    library_compile_owner_drop(&owner);
    puts("additional complete long-literal evaluation packet: zero/copy/SHA/compare4096, batch70 prefix/final rollback and kept String retirement PASS");
}
static void specialize_generic_controls(void) {
    LibraryCompileOwner source_owner={0};CHECK(library_compile_owner_new(&source_owner,&library_compile_limits)==XR_XIR_OK);
    char text[8000];size_t bytes=implementation_control_fixture(text,sizeof(text));XrXirArtifact *source=NULL;
    CHECK(specialize_source(&source_owner.context,text,bytes,NULL,&source)==XR_XIR_OK);
    uint64_t held=library_compile_stats(&source_owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirGeneric *copy=NULL;
        specialize_begin(&probe,&control,source_owner.context.resources,"zero-prefix",mode);
        XrXirStatus status=xr_xir_compile_generics_clone(&source_owner.context,&source->module,&copy,&control);
        specialize_end(&probe,status);CHECK(!copy&&library_compile_stats(&source_owner.context).held_bytes==held);
    }
    XrXirGeneric *copy=NULL;source_program_compile_fail_at=source_program_compile_attempts;source_program_compile_injected=false;
    CHECK(xr_xir_compile_generics_clone(&source_owner.context,&source->module,&copy,NULL)==XR_XIR_OUT_OF_MEMORY);
    source_program_compile_fail_at=SIZE_MAX;CHECK(source_program_compile_injected&&!copy&&library_compile_stats(&source_owner.context).held_bytes==held);
    LibraryCompileOwner receiver={0};CHECK(library_compile_owner_new(&receiver,&library_compile_limits)==XR_XIR_OK);
    CHECK(xr_xir_compile_generics_clone(&receiver.context,&source->module,&copy,NULL)==XR_XIR_OK);
    XrCompileResourceStats required=library_compile_stats(&receiver.context);
    xr_xir_compile_generics_free(copy,source->module.function_count);copy=NULL;library_compile_owner_drop(&receiver);
    for(unsigned point=0;point<3;++point) {
        XrCompileResourceLimits limits=library_compile_limits;CHECK(required.peak_bytes>1);
        limits.memory_limit=required.peak_bytes-1+point;
        CHECK(library_compile_owner_new(&receiver,&limits)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_generics_clone(&receiver.context,&source->module,&copy,NULL);
        CHECK(status==(point==0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        if(copy)xr_xir_compile_generics_free(copy,source->module.function_count);copy=NULL;
        library_compile_owner_drop(&receiver);
    }
    CHECK(library_compile_stats(&source_owner.context).held_bytes==held);
    xr_xir_compile_artifact_free(source);library_compile_owner_drop(&source_owner);
    puts("complete7658 generic metadata real zero4096 controls/physicalOOM and foreign held-minus/exact/plus rollback PASS");
}
