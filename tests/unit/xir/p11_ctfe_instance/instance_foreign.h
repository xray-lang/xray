/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * instance_foreign.h - Actual private witness edges retain their definitions
 */
static XrXirStatus instance_foreign_source(const XrXirSourceRequest *original,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic,XirSourceInstanceConstants *constants,
    const XirSourceInstanceSelection *selections,uint32_t count) {
    static const char root[]="import {foreign} from \"./generic\"\n"
        "export fn run()->i64{return foreign<i8>()+foreign<string>()-42}\n";
    static const char protocol[]="export interface Getter{get()->i64}\n";
    static const char generic[]="import {Getter} from \"./protocol\"\nimport {Holder} from \"./owner\"\n"
        "fn apply<U:Getter>(value:U)->i64{return value.get()}\n"
        "export fn foreign<T>()->i64{return apply<Holder<T>>(Holder<T>.make())}\n";
    static const char owner[]="import {Getter} from \"./protocol\"\n"
        "export struct Holder<T> implements Getter{private value:T?;"
        "static make()->Holder<T>{return Holder<T>{value:null}};get()->i64{const x=this.value;return 42}}\n";
    const char *names[]={"root.xr","protocol.xr","generic.xr","owner.xr"};
    const char *texts[]={root,protocol,generic,owner};char paths[4][1024];XrModuleOverlayInput inputs[4];
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    for (unsigned i=0;i<4;++i) {
        int length=snprintf(paths[i],sizeof(paths[i]),"%s/%s",MATERIALIZATION_ROOT,names[i]);
        CHECK(length>0 && (size_t)length<sizeof(paths[i]));
        inputs[i]=(XrModuleOverlayInput){authority,names[i],paths[i],texts[i],strlen(texts[i])};
    }
    XrXirSourceRequest request=*original;request.entry_path=paths[0];request.authority=&authority;
    if (constants) {
        XirSourceInstanceRequest input={.source=&request,.overlays=inputs,.overlay_count=4,.selections=selections,.count=count,
            .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        return xr_xir_compile_source_instance_constants(&input,constants,diagnostic);
    }
    return xr_xir_compile_source_check_overlay(&request,inputs,4,output,diagnostic,NULL);
}
static void instance_foreign_edges(const XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;unsigned edges=0;
    for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[f].instructions[i];if (op->op!=XR_XIR_CALL) continue;
        const XrXirFunctionIdentity *a=&module->declarations->functions[f],*b=&module->declarations->functions[op->immediate];
        if (a->module!=b->module && !b->exported) ++edges;
    }
    CHECK(edges>=2);fprintf(stderr,"instance genuine cross-module private witness edges=%u\n",edges);
}
