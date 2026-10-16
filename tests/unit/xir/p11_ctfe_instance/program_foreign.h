/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_foreign.h - Public generic comptime retains genuine private witnesses
 */
static XrXirStatus program_foreign_source(XrXirCompileContext *context,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    static const char root[]="import {foreign} from \"./generic\"\nexport fn run()->i64{return foreign<i8>()+foreign<string>()-42}\n";
    static const char protocol[]="export interface Getter{get()->i64}\n";
    static const char generic[]="import {Getter} from \"./protocol\"\nimport {Holder} from \"./owner\"\n"
        "fn apply<U:Getter>(value:U)->i64{return value.get()}\n"
        "export fn foreign<T>()->i64{return comptime apply<Holder<T>>(Holder<T>.make())}\n";
    static const char owner[]="import {Getter} from \"./protocol\"\nexport struct Holder<T> implements Getter{private value:T?;"
        "static make()->Holder<T>{return Holder<T>{value:null}};get()->i64{const x=this.value;return 42}}\n";
    const char *names[]={"root.xr","protocol.xr","generic.xr","owner.xr"},*texts[]={root,protocol,generic,owner};
    char paths[4][1024];XrModuleOverlayInput inputs[4];
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    for (unsigned i=0;i<4;++i) {
        int length=snprintf(paths[i],sizeof(paths[i]),"%s/%s",MATERIALIZATION_ROOT,names[i]);
        CHECK(length>0 && (size_t)length<sizeof(paths[i]));
        inputs[i]=(XrModuleOverlayInput){authority,names[i],paths[i],texts[i],strlen(texts[i])};
    }
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrXirSourceRequest request={session,paths[0],&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,inputs,4,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
