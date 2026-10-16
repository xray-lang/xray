/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derived_library_import_controls.h - Exact cold library identity boundaries
 */
#ifndef XIR_SOURCE_DERIVED_LIBRARY_IMPORT_CONTROLS_H
#define XIR_SOURCE_DERIVED_LIBRARY_IMPORT_CONTROLS_H
static void library_catalog_reject(const XrXirCheckedPacket *packet,const XrXirLibraryModuleInput bindings[2],bool duplicate) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context=library_context();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirLibraryModuleInput wrong[2];memcpy(wrong,bindings,sizeof(wrong));
    if(!duplicate) {
        unsigned changed=0;
        for(unsigned i=0;i<2;++i)if(!strcmp(wrong[i].logical_path,"derived_library.xr")){wrong[i].logical_path="misbound_library.xr";++changed;}
        CHECK(changed==1);
    }
    XrXirLibraryInput inputs[2]={{packet->bytes,packet->length,{0},wrong,2},{packet->bytes,packet->length,{0},wrong,2}};
    xr_sha256(packet->bytes,packet->length,inputs[0].sha256);memcpy(inputs[1].sha256,inputs[0].sha256,32);
    XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&context,inputs,duplicate?2:1,&catalog);
    fprintf(stderr,"Derived catalog identity duplicate%u status%u\n",(unsigned)duplicate,(unsigned)status);
    CHECK(status==XR_XIR_BAD_STRUCTURE && !catalog);XrCompileResourceStats spent={0};
    CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK && spent.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes && !runtime_live && !runtime_bytes);
}
static int library_import_controls(void) {
    XrXirCompileContext transport=library_context();XrXirLibraryModuleInput bindings[2];
    XrXirCheckedPacket packet=library_fixture(&transport,bindings);library_catalog_reject(&packet,bindings,false);
    library_catalog_reject(&packet,bindings,true);XrXirCompileContext contexts[2];XrXirLibraryCatalog *catalogs[2];
    for(unsigned i=0;i<2;++i){contexts[i]=library_context();catalogs[i]=library_catalog(&contexts[i],&packet,bindings,2);}
    memset(bindings,0xa5,sizeof(bindings));memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(transport.resources);
    const char *dependency="import {generate,readGenerate} from \"./constructor_names\"\n";
    const char *end=strchr(downstream_source,'\n');CHECK(end);size_t first=(size_t)(end+1-downstream_source);
    for(unsigned i=0;i<2;++i) {
        char text[8192],logical[64];int n;
        if(!i)n=snprintf(text,sizeof(text),"%s%s",dependency,downstream_source);
        else n=snprintf(text,sizeof(text),"%.*s%s%s",(int)first,downstream_source,dependency,end+1);
        CHECK(n>0 && n<(int)sizeof(text));n=snprintf(logical,sizeof(logical),"downstream-order%u.xr",i);CHECK(n>0 && n<(int)sizeof(logical));
        XrXirSourceResult result={0};CHECK(library_source(&contexts[i],catalogs[i],logical,text,XR_XIR_PROGRAM,&result)==XR_XIR_OK);
        memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(catalogs[i]);
        const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);const XrXirDerivationTable *grants=module->declarations->derivations;
        CHECK(module->declarations->module_count==3 && grants && grants->policy_count==2 && grants->count==2 &&
            !grants->policies[0].member_count && grants->policies[0].constructor && !grants->policies[1].constructor &&
            grants->policies[1].member_count==1 && grants->policies[1].members[0].field==0 &&
            grants->policies[1].members[0].operations==(XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ));
        CHECK(grants->records[0].function<grants->records[1].function && grants->records[0].policy==0 && grants->records[1].policy==1);
        XrXirValue retained=library_execute(&contexts[i],&result,NULL,NULL);xr_compile_resources_release(contexts[i].resources);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&retained,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));
        xr_xir_value_drop(&retained);printf("Derived direct generator import order%u: original two policies/grants, exact private READ42 and final String\n",i);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    puts("Derived cold catalog rejects duplicate identities and false module bindings; both actual import orders preserve sole grants and physical0");return 0;
}
#endif // XIR_SOURCE_DERIVED_LIBRARY_IMPORT_CONTROLS_H
