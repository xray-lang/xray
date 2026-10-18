static void recursive_metadata_controls(void) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_ARRAY,.element=256},{.kind=XR_XIR_TYPE_ARRAY,.element=256}};
    XrXirTypes types={nodes,1,NULL,NULL};XrXirCompileContext context;context_new(&context);
    for (unsigned kind=0;kind<4;++kind) {
        nodes[0].kind=kind==2?XR_XIR_TYPE_NULLABLE:XR_XIR_TYPE_ARRAY;
        nodes[0].element=kind==1 || kind==3?257:256;types.count=kind==1 || kind==3?2:1;
        nodes[1].kind=kind==3?XR_XIR_TYPE_NULLABLE:XR_XIR_TYPE_ARRAY;
        XrXirCtfeTypeShape shape={123,true,true};
        XrCompileResourceStats before=stats(&context);
        XrXirCtfeStatus status=xr_xir_compile_ctfe_type_shape(&context,&types,256,&shape);
        if (status!=XR_XIR_CTFE_NOT_SUPPORTED)
            fprintf(stderr,"Recursive synthetic metadata kind=%u status=%u nodes=%u rootkind=%u childkind=%u\n",kind,status,types.count,nodes[0].kind,nodes[1].kind);
        CHECK(status==XR_XIR_CTFE_NOT_SUPPORTED);
        CHECK(shape.work==123 && shape.dynamic && shape.pure_arrays);
        CHECK(stats(&context).live_bytes==before.live_bytes && stats(&context).work>before.work);
    }
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=256};types.count=2;
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
    XrXirCtfeTypeShape shape={0};
    CHECK(xr_xir_compile_ctfe_type_shape(&context,&types,257,&shape)==XR_XIR_CTFE_OK && shape.dynamic && !shape.pure_arrays && shape.work);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Malformed self/mutual Array and Nullable descriptors refuse with preserved shape; canonical Array<Nullable<i64>> admits, genuine recursive nominal Source is checked separately, physical0\n");
}
static void recursive_family_controls(const char *fixture,bool reject_export) {
    RecursiveInput input;recursive_load(&input,fixture,reject_export,true);
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=recursive_source(&context,&input,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Recursive family %s plain=%u status=%u %d:%d %s\n",fixture,reject_export,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    XrXirType type=module->functions[named(module,"factory")].result;
    XrXirCtfeTypeShape shape={123,true,true};XrCompileResourceStats before=stats(&context);
    CHECK(xr_xir_compile_ctfe_type_shape(&context,module->types,type,&shape)==XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(shape.work==123 && shape.dynamic && shape.pure_arrays && stats(&context).live_bytes==before.live_bytes);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *lowered=NULL;
    CHECK(recursive_pipeline(&context,&packet,&lowered)==XR_XIR_OK);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(context.resources);recursive_execute(&lowered);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    if (reject_export) {
        recursive_load(&input,fixture,false,true);context_new(&context);memset(&diagnostic,0,sizeof(diagnostic));
        status=recursive_source(&context,&input,&source,&diagnostic);
        fprintf(stderr,"Recursive real export %s status=%u %d:%d %s\n",fixture,status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot && strstr(diagnostic.message,"closed value family"));
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Recursive Source family %s: genuine checked type still rejects identity export after gray backedge; independent packet/two Instance42/physical0\n",fixture);
}
