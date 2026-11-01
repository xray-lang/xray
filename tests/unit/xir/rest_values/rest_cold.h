/* Owned Checked libraries retain rest types after each producer retires. */
#ifndef XR_REST_COLD_H
#define XR_REST_COLD_H
static XrXirSourceResult rest_cold_source(const XrXirCompileContext *context,
    const XrXirLibraryCatalog *catalog,const char *name,const char *text,XrXirLinkageKind kind) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_REST_WORK,name)>0);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REST_WORK};
    const XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,
        .context=context,.linkage_kind=kind,.libraries=catalog};
    const XrXirSourceText input={name,text,strlen(text)};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"cold %s status%u %d:%d %s\n",name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);return result;
}
static XrXirCheckedPacket rest_cold_packet(XrXirSourceResult *source,const XrXirCompileContext *context,uint64_t before) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source->snapshot);unsigned rest=0;
    for(uint32_t i=0;i<view->declaration_count;++i)if(view->declarations[i].flags&XR_XIR_SOURCE_DECL_REST) {
        const XrXirSourceDeclaration *declaration=&view->declarations[i];CHECK(declaration->parameter_count);
        const XrXirSourceType *tail=&declaration->parameters[declaration->parameter_count-1];
        CHECK(tail->known&&xr_xir_type_is_array(view->types,tail->type));++rest;
    }
    CHECK(rest>=2);unsigned char *bytes=xr_malloc(packet.length);CHECK(bytes);
    size_t length=packet.length;memcpy(bytes,packet.bytes,length);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(source);
    CHECK(held(context->resources)==before);return (XrXirCheckedPacket){bytes,length};
}
static XrXirLibraryCatalog *rest_cold_catalog(XrXirCompileContext *context,XrXirCheckedPacket *packet,
    uint32_t count) {
    char root[1024];CHECK(snprintf(root,sizeof(root),"%s",XR_REST_WORK)>0);
    char names[2][24]={"base.xr","bridge.xr"};XrXirLibraryModuleInput modules[2]={{0}};
    XrXirArtifact *checked=NULL;CHECK(count&&count<=2);
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    CHECK(module->declarations->module_count==count);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    for(uint32_t n=0;n<count;++n) {
        char *identity=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,names[n],&identity)==XR_MODULE_OK);
        unsigned matched=0;
        for(uint32_t m=0;m<count;++m) {
            const XrXirSourceModule *view=&module->declarations->modules[m];
            if(view->name_length==strlen(identity)&&!memcmp(view->name,identity,view->name_length)) {
                CHECK(!modules[m].logical_path);modules[m]=(XrXirLibraryModuleInput){authority,names[n]};++matched;
            }
        }
        CHECK(matched==1);xr_compile_resources_free(identity);
    }
    XrXirLibraryInput input={packet->bytes,packet->length,{0},modules,count};
    xr_sha256(packet->bytes,packet->length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus catalog_status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if(catalog_status!=XR_XIR_OK)fprintf(stderr,"rest cold catalog status%d\n",(int)catalog_status);
    CHECK(catalog_status==XR_XIR_OK&&catalog);
    xr_xir_compile_artifact_free(checked);memset(packet->bytes,0xa5,packet->length);xr_free(packet->bytes);
    *packet=(XrXirCheckedPacket){0};memset(names,0xa5,sizeof(names));memset(root,0xa5,sizeof(root));return catalog;
}
static XrXirSourceResult rest_cold_two_hops(XrXirCompileContext *receiver) {
    XrXirCompileContext producer=context_new();uint64_t producer_before=held(producer.resources);
    static const char base[]=
        "export fn sum(...xs:i64)->i64{var n=0;for(x in xs){n+=x};return n}\n"
        "export fn ordinary(xs:Array<i64>)->i64{var n=0;for(x in xs){n+=x};return n}\n"
        "export fn collect<T>(...xs:T)->Array<T>{return xs}\n"
        "export fn produced<T>(...xs:T)->Iterator<T>{for(x in xs){yield x}}\n"
        "export fn prefixed(head:i64=40,...xs:i64)->i64{return head+sum(...xs)}\n"
        "export fn selected()->fn(...i64)->i64{return sum}\n";
    XrXirSourceResult source=rest_cold_source(&producer,NULL,"base.xr",base,XR_XIR_LIBRARY);
    XrXirCheckedPacket packet=rest_cold_packet(&source,&producer,producer_before);xr_compile_resources_release(producer.resources);
    XrXirCompileContext middle=context_new();uint64_t middle_before=held(middle.resources);
    XrXirLibraryCatalog *catalog=rest_cold_catalog(&middle,&packet,1);
    static const char bridge[]=
        "import {sum,collect,selected,produced} from \"./base\"\n"
        "export fn bridge(...xs:i64)->i64{return sum(...xs)}\n"
        "export fn returned()->fn(...i64)->i64{return selected()}\n"
        "export fn producer()->fn(...i64)->Iterator<i64>{return produced<i64>}\n"
        "export fn texts(...xs:string)->Array<string>{return collect<string>(...xs)}\n";
    source=rest_cold_source(&middle,catalog,"bridge.xr",bridge,XR_XIR_LIBRARY);
    xr_xir_compile_library_catalog_free(catalog);packet=rest_cold_packet(&source,&middle,middle_before);
    xr_compile_resources_release(middle.resources);catalog=rest_cold_catalog(receiver,&packet,2);
    static const char consumer[]=
        "import {sum,ordinary,prefixed} from \"./base\"\n"
        "import {bridge,returned,texts,producer} from \"./bridge\"\n"
        "fn invoke(f:fn(...i64)->i64)->i64{return f(20,...[22])}\n"
        "export fn referenceAnswer()->i64{const f=returned();const direct:fn(...i64)->i64=sum;"
        "const fixed:fn(Array<i64>)->i64=ordinary;const data=texts(\"A\\0中\",\"B\");"
        "assert(len(data)==2&&data[0]==\"A\\0中\");"
        "assert(bridge(1,2,...[3])==6);assert(fixed([20,22])==42);assert(direct(20,22)==42);"
        "assert(prefixed()==40&&prefixed(1,2,...[3])==6);"
        "const generate=producer();var n=0;for(x in generate(20,...[22])){n+=x};assert(n==42);return invoke(f)}\n";
    source=rest_cold_source(receiver,catalog,"consumer.xr",consumer,XR_XIR_PROGRAM);
    xr_xir_compile_library_catalog_free(catalog);return source;
}
#endif
