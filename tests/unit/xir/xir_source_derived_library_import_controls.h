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
        XrXirValue retained=library_execute(&contexts[i],&result,NULL,NULL,false);xr_compile_resources_release(contexts[i].resources);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&retained,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));
        xr_xir_value_drop(&retained);printf("Derived direct generator import order%u: original two policies/grants, exact private READ42 and final String\n",i);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    puts("Derived cold catalog rejects duplicate identities and false module bindings; both actual import orders preserve sole grants and physical0");return 0;
}
static void library_generator_hidden(const XrXirCheckedPacket *packet,XrXirLibraryModuleInput bindings[2]) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context=library_context();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirLibraryCatalog *catalog=library_catalog(&context,packet,bindings,2);
    const char *logical="downstream-generator-hidden.xr";library_absent(logical);char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_DERIVED_LIBRARY_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DERIVED_LIBRARY_WORK};
    XrXirSourceRequest request={session,path,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    char text[]="import {generate} from \"./constructor_names\"\nexport fn probe(value:Future)->i64{return 0}\nexport fn main()->i64{return 42}\n";
    XrXirSourceText input={logical,text,strlen(text)};XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&output,&diagnostic,NULL);
    fprintf(stderr,"Unselected Future type refusal%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && diagnostic.line==2 && diagnostic.column==23 && !output.checked && !output.snapshot &&
        strstr(diagnostic.message,"name does not resolve to an admitted nominal type"));
    memset(text,0xa5,sizeof(text));xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK && spent.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes && !runtime_live && !runtime_bytes);
}
static int library_generator_only(const char *emit,const char *packet_path) {
    XrXirCompileContext transport=library_context();XrXirLibraryModuleInput bindings[2];
    XrXirCheckedPacket packet=library_fixture(&transport,bindings);library_generator_hidden(&packet,bindings);
    XrXirCompileContext context=library_context();
    XrXirLibraryCatalog *catalog=library_catalog(&context,&packet,bindings,2);
    memset(bindings,0xa5,sizeof(bindings));memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(transport.resources);
    char text[]=
        "import {generate} from \"./constructor_names\"\n"
        "interface CtorNames {parameterNames()->string}\n"
        "@derive(CtorNames,generate,construct:true)\n"
        "struct Later {private const total:i64\n"
        " private init(right:i64,left:i64){this.total=right+left}\n"
        " static seed()->Later{return Later(9,7)} }\n"
        "export fn names()->string{return Later.seed().parameterNames()}\n"
        "export fn main()->i64{const folded=comptime Later.seed().parameterNames();return (folded==\"right:left\" && names()==folded)?42:0}\n";
    XrXirSourceResult source={0};XrXirStatus status=library_source(&context,catalog,"downstream-generator-only.xr",text,XR_XIR_PROGRAM,&source);
    memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(catalog);
    if(status!=XR_XIR_OK) {
        xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
        fprintf(stderr,"Generator-only failure cleaned all compiler/runtime owners before original success assertion\n");
    }
    CHECK(status==XR_XIR_OK);const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirDerivationTable *grants=module->declarations->derivations;
    CHECK(module->declarations->module_count==2 && grants && grants->policy_count==1 && grants->count==1 &&
        grants->policies[0].constructor && !grants->policies[0].member_count && grants->records[0].policy==0);
    const XrXirNominalTable *nominals=module->types->nominals;
    for(uint32_t n=0;nominals && n<nominals->count;++n) {
        XrXirLiteral name=nominals->declarations[n].name;CHECK(name.length!=6 || memcmp(name.bytes,"Future",6));
    }
    const XrXirInterfaceTable *interfaces=module->types->interfaces;
    for(uint32_t i=0;interfaces && i<interfaces->count;++i) {
        XrXirLiteral name=interfaces->declarations[i].name;CHECK(name.length!=7 || memcmp(name.bytes,"Measure",7));
    }
    XrXirValue retained=library_execute(&context,&source,emit,packet_path,true);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Generator-only receiver complete allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    xr_compile_resources_release(context.resources);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&retained,&bytes,&length) && length==10 && !memcmp(bytes,"right:left",10));
    xr_xir_value_drop(&retained);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    puts("Generator-only cold view excludes unselected Future policies, admits exactly local constructor grant, comptime42 and final String/physical0");return 0;
}
#endif // XIR_SOURCE_DERIVED_LIBRARY_IMPORT_CONTROLS_H
