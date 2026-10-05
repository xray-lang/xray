/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_source_admission_cases.h - Governed Source Atomic and Ordering admission boundaries
 */
#ifndef XIR_ATOMIC_SOURCE_ADMISSION_CASES_H
#define XIR_ATOMIC_SOURCE_ADMISSION_CASES_H
enum { AS_ORDERING=1,AS_ATOMIC=2,AS_CAS=4,AS_ORDINARY_ORDERING=8,AS_ORDINARY_ATOMIC=16,AS_PRIVATE_PERMISSION=32 };
typedef struct AtomicSourceCase {const char *name,*source;XrXirStatus expected;unsigned query;} AtomicSourceCase;
static void atomic_source_query(const XrXirSourceView *view,unsigned expected){
    CHECK(view && view->complete && view->types);unsigned found=0;
    for(uint32_t i=0;i<view->declaration_count;++i){
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_TYPE && !strcmp(d->name,"Ordering") && d->native_identity==XR_NATIVE_DECLARATION_ORDERING){
            CHECK(d->exported && d->type.known && xr_xir_nominal_native_ordering(view->types,d->type.type));found|=AS_ORDERING;
        }
        if(d->kind==XR_XIR_SOURCE_TYPE && !strcmp(d->name,"Atomic") && d->native_identity==XR_NATIVE_DECLARATION_ATOMIC){
            CHECK(d->generic_parameter_count==1 && d->generic_constraints && d->generic_constraints[0].markers==XR_XIR_CONSTRAINT_ATOMIC_VALUE);found|=AS_ATOMIC;
        }
        if(d->kind==XR_XIR_SOURCE_MEMBER && !strcmp(d->name,"compareExchange")){
            const XrXirTypeNode *tuple=xr_xir_type_node(view->types,d->type.type);
            CHECK(d->type.known && tuple && tuple->kind==XR_XIR_TYPE_TUPLE && tuple->parameter_count==2);
            CHECK(tuple->parameters[0].type==XR_XIR_TYPE_PARAMETER_BASE && tuple->parameters[1].type==XR_XIR_BOOL);
            CHECK(d->parameter_count==3 && d->parameters[2].known);
            XrXirType ord=d->parameters[2].type;CHECK(xr_xir_type_is_nullable(view->types,ord));
            CHECK(xr_xir_nominal_native_ordering(view->types,xr_xir_nullable_element(view->types,ord)));found|=AS_CAS;
        }
        if(d->range.module==0 && !strcmp(d->name,"Ordering")){
            CHECK(d->native_identity==0);found|=AS_ORDINARY_ORDERING;
        }
        if(d->range.module==0 && !strcmp(d->name,"Atomic")){
            CHECK(d->native_identity==0);found|=AS_ORDINARY_ATOMIC;
        }
    }
    CHECK((found&expected)==expected);
}
static XrXirStatus atomic_source_control(const XrXirCompileContext *context,void *opaque){
    AtomicSourceCase *c=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus cs=xr_compile_session_new(context->resources,&session);
    if(cs!=XR_COMPILER_SESSION_OK)return cs==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory_path};
    XrXirSourceRequest request={session,root_path,&authority,context,XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status==XR_XIR_OK)CHECK(source.checked && source.snapshot);
    else CHECK(!source.checked && !source.snapshot);
    if(c->query&AS_PRIVATE_PERMISSION)CHECK(status==XR_XIR_BAD_STRUCTURE && strstr(diagnostic.message,"import requires an exported declaration"));
    xr_compile_session_free(session);
    if(status==XR_XIR_OK)atomic_source_query(xr_xir_compile_source_snapshot_view(source.snapshot),c->query);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_xir_compile_source_result_free(&source);
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(specialized);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    xr_xir_compile_artifact_free(lowered);
    if(status!=c->expected)fprintf(stderr,"Source %s status%u expected%u at%u:%d:%d %s\n",c->name,status,c->expected,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==c->expected);return status;
}
static void atomic_source_controls(void){
    const char *orders[]={"Relaxed","Acquire","Release","AcquireRelease","SeqCst"};
    for(unsigned i=0;i<5;++i){char source[256];CHECK(snprintf(source,sizeof(source),"const a=Atomic(7)\nconst p=a.compareExchange(7,8,Ordering.%s)\n",orders[i])>0);
        AtomicSourceCase c={orders[i],source,XR_XIR_OK,AS_ORDERING|AS_ATOMIC|AS_CAS};write_source(source);
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);CHECK(atomic_source_control(&owner.context,&c)==XR_XIR_OK);library_compile_owner_drop(&owner);}
    AtomicSourceCase cases[]={
        {"null order","const a=Atomic(7)\na.load(null)\na.store(8,null)\n",XR_XIR_OK,AS_ORDERING|AS_ATOMIC},
        {"dynamic nullable order","fn get(a:Atomic<i64>,ord:Ordering?)->i64{return a.load(ord)}\n",XR_XIR_OK,AS_ORDERING|AS_ATOMIC},
        {"load Release","const a=Atomic(7)\na.load(Ordering.Release)\n",XR_XIR_BAD_TYPE,0},
        {"load AcquireRelease","const a=Atomic(7)\na.load(Ordering.AcquireRelease)\n",XR_XIR_BAD_TYPE,0},
        {"store Acquire","const a=Atomic(7)\na.store(8,Ordering.Acquire)\n",XR_XIR_BAD_TYPE,0},
        {"store AcquireRelease","const a=Atomic(7)\na.store(8,Ordering.AcquireRelease)\n",XR_XIR_BAD_TYPE,0},
        {"ordinary Ordering type","enum Ordering { SeqCst }\nfn same(x:Ordering)->Ordering{return x}\n",XR_XIR_OK,AS_ORDINARY_ORDERING},
        {"ordinary Ordering cannot authorize","enum Ordering { SeqCst }\nconst a=Atomic(7)\na.load(Ordering.SeqCst)\n",XR_XIR_BAD_TYPE,0},
        {"ordinary Atomic function","fn Atomic(n:i64)->i64{return n}\nconst x=Atomic(7)\n",XR_XIR_OK,AS_ORDINARY_ATOMIC},
        {"local Atomic function","fn run()->i64 {const Atomic=fn(n:i64)->i64{return n}; return Atomic(7)}\n",XR_XIR_OK,AS_ORDINARY_ATOMIC},
        {"local Ordering value","fn run()->i64 {const Ordering=7;const a=Atomic(1);return a.load(Ordering)}\n",XR_XIR_BAD_TYPE,0},
        {"generic Ordering cannot authorize","fn unused<Ordering>(a:Atomic<i64>,ord:Ordering)->i64{return a.load(ord)}\n",XR_XIR_BAD_TYPE,0},
        {"generic Atomic cannot construct","fn unused<Atomic>() {const x=Atomic(1)}\n",XR_XIR_BAD_TYPE,0},
        {"Number definition","fn bump<T>(a:Atomic<T>,v:T)->T where T:AtomicNumber{return a.fetchAdd(v)}\nconst f=bump<i64>\n",XR_XIR_OK,AS_ATOMIC|AS_ORDERING},
        {"Boolean definition","fn flip<T>(a:Atomic<T>)->bool where T:AtomicBoolean{return a.toggle()}\nconst f=flip<bool>\n",XR_XIR_OK,AS_ATOMIC|AS_ORDERING},
        {"Value not Number","fn unused<T:AtomicValue>(a:Atomic<T>,v:T)->T{return a.fetchAdd(v)}\n",XR_XIR_BAD_TYPE,0},
        {"Value not Boolean","fn unused<T:AtomicValue>(a:Atomic<T>)->bool{return a.toggle()}\n",XR_XIR_BAD_TYPE,0},
        {"Number not Boolean","fn unused<T:AtomicNumber>(a:Atomic<T>)->bool{return a.toggle()}\n",XR_XIR_BAD_TYPE,0},
        {"Boolean not Number","fn unused<T:AtomicBoolean>(a:Atomic<T>,v:T)->T{return a.fetchAdd(v)}\n",XR_XIR_BAD_TYPE,0},
        {"private enum permission","import \"./hidden\" as lib\nconst a=Atomic(1)\na.load(lib.Hidden.V)\n",XR_XIR_BAD_STRUCTURE,AS_PRIVATE_PERMISSION},
        {"export fake enum no authority","import \"./hidden\" as lib\nconst a=Atomic(1)\na.load(lib.Fake.V)\n",XR_XIR_BAD_TYPE,0}
    };
    unsigned good=5,bad=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){write_source(cases[i].source);LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        CHECK(atomic_source_control(&owner.context,&cases[i])==cases[i].expected);library_compile_owner_drop(&owner);if(cases[i].expected==XR_XIR_OK)++good;else++bad;}
    fprintf(stderr,"Atomic Source controls positive%u negative%u finite owners physical0; runtime/native NOT_RUN\n",good,bad);
}
#endif
