/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_class_inheritance_source.c - Cold defining libraries and derived consumers
 *
 * KEY CONCEPT:
 *   The defining Source owner retires before a new Source consumer derives from
 *   the catalog class. Fresh Checked and runtime receivers retain full identity.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "base/xio_policy.h"
#include "os/os_fs.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"class inheritance %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "class_inheritance_cases.h"

typedef struct ClassInheritanceSource {
    const char *text,*logical,*root;
    XrXirLinkageKind kind;
    const XrXirLibraryCatalog *catalog;
    XrCompileRequestControl *control;
} ClassInheritanceSource;
static uint64_t class_inheritance_held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats.held_bytes;
}
static XrXirStatus class_inheritance_check(const XrXirCompileContext *context,
    const ClassInheritanceSource *fixture,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(fixture->control)CHECK(xr_compile_session_bind_request(session,fixture->control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={fixture->root?XR_MODULE_IDENTITY_SCRIPT:XR_MODULE_IDENTITY_MEMORY,
        fixture->root?NULL:"class-inheritance-control",fixture->root};
    char path[2048];const char *entry=NULL;
    if(fixture->root) {
        int length=snprintf(path,sizeof(path),"%s/%s",fixture->root,fixture->logical);
        CHECK(length>0 && (size_t)length<sizeof(path));entry=path;
    }
    XrXirSourceRequest request={session,entry,&authority,context,XR_CLASS_INHERITANCE_ROOT "/stdlib",
        NULL,fixture->kind,fixture->catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
    if(fixture->text) {
        XrXirSourceText input={fixture->root?fixture->logical:NULL,fixture->text,strlen(fixture->text)};
        status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    } else status=xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    fprintf(stderr,"inheritance Source %s status%u %d:%d %s\n",fixture->logical,
        status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);
    if(status!=XR_XIR_OK)CHECK(!result->checked && !result->snapshot);
    return status;
}
static XrXirArtifact *class_inheritance_receive(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    return lowered;
}
static const char class_inheritance_base[]=
    "export class Animal {\n"
    " name:string\n number:i64\n"
    " init(name:string,number:i64){this.name=name;this.number=number}\n"
    " speak()->i64{return this.number}\n}\n"
    "export fn bind(value:Animal)->fn()->i64{return value.speak}\n";
static const char class_inheritance_consumer[]=
    "import {Animal,bind} from \"./base\"\n"
    "class Dog extends Animal {\n"
    " tail:Array<string>\n"
    " init(name:string,number:i64){super(name,number);this.tail=[\"derived-owned-tail-one\",\"derived-owned-tail-two\"]}\n"
    " override speak()->i64{return this.number+len(this.tail)}\n}\n"
    "fn retained()->fn()->i64{\n"
    " var receiver:Animal=Dog(\"Rex-owned-name\",40)\n"
    " const bound=bind(receiver)\n"
    " receiver=Animal(\"Other-owned-name\",0)\n"
    " return bound\n}\n"
    "export fn answer()->i64{const held=retained();assert(held()==42);print(\"cold receiver passed\");return 42}\n";
static XrXirLibraryCatalog *class_inheritance_catalog_receive(const XrXirCompileContext *context,
    XrXirSourceResult *source,const char *root,const char *const *names,uint32_t count) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    CHECK(count<=4 && module->declarations->module_count==count && module->linkage_kind==XR_XIR_LIBRARY);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirLibraryModuleInput bindings[4]={0};
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *actual=module->declarations->modules+m;bool found=false;
        for(uint32_t n=0;n<count;++n) {
            char *canonical=NULL;
            CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,names[n],&canonical)==XR_MODULE_OK);
            bool same=actual->name_length==strlen(canonical) && !memcmp(actual->name,canonical,actual->name_length);
            xr_compile_resources_free(canonical);if(!same)continue;
            CHECK(!found);found=true;bindings[m]=(XrXirLibraryModuleInput){authority,names[n]};
        }
        CHECK(found);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,count};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);return catalog;
}
static XrXirLibraryCatalog *class_inheritance_catalog(const XrXirCompileContext *context,const char *root) {
    char absent[2048];int length=snprintf(absent,sizeof(absent),"%s/base.xr",root);
    CHECK(length>0 && (size_t)length<sizeof(absent));FILE *fallback=fopen(absent,"rb");
    if(fallback){CHECK(!fclose(fallback));CHECK(false);}
    ClassInheritanceSource fixture={class_inheritance_base,"base.xr",root,XR_XIR_LIBRARY,NULL,NULL};
    XrXirSourceResult source={0};CHECK(class_inheritance_check(context,&fixture,&source)==XR_XIR_OK);
    static const char *const names[]={"base.xr"};
    return class_inheritance_catalog_receive(context,&source,root,names,1);
}
static XrXirLibraryCatalog *class_inheritance_hidden_catalog(const XrXirCompileContext *context,const char *root,
    const XrXirLibraryCatalog *base) {
    static const char dog[]=
        "import {Animal} from \"./base\"\n"
        "class Dog extends Animal{tail:Array<string>\n"
        " init(name:string,number:i64){super(name,number);this.tail=[\"derived-owned-tail-one\",\"derived-owned-tail-two\"]}\n"
        " override speak()->i64{return this.number+len(this.tail)}\n}\n"
        "export fn make()->Animal{return Dog(\"Rex-owned-name\",40)}\n";
    static const char client[]=
        "import {Animal} from \"./base\"\n"
        "export fn retained(value:Animal)->fn()->i64{\n"
        " var receiver=value\n const bound=receiver.speak\n"
        " receiver=Animal(\"Other-owned-name\",0)\n return bound\n}\n";
    static const char facade[]=
        "import {make} from \"./dog\"\nimport {retained} from \"./client\"\n"
        "export fn callHidden()->i64{const bound=retained(make());return bound()}\n";
    const char *names[]={"dog.xr","client.xr","facade.xr"},*texts[]={dog,client,facade};
    char paths[3][2048];XrModuleOverlayInput overlays[3];
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    for(unsigned i=0;i<3;++i) {
        int length=snprintf(paths[i],sizeof(paths[i]),"%s/%s",root,names[i]);CHECK(length>0 && (size_t)length<sizeof(paths[i]));
        overlays[i]=(XrModuleOverlayInput){authority,names[i],paths[i],texts[i],strlen(texts[i])};
    }
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,paths[2],&authority,context,XR_CLASS_INHERITANCE_ROOT "/stdlib",
        NULL,XR_XIR_LIBRARY,base};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,overlays,3,&source,&diagnostic,NULL);
    fprintf(stderr,"inheritance hidden Library status%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);CHECK(status==XR_XIR_OK);
    static const char *const owned_names[]={"base.xr","dog.xr","client.xr","facade.xr"};
    return class_inheritance_catalog_receive(context,&source,root,owned_names,4);
}
static XrXirNominalIdentity class_inheritance_header(const XrXirNominalTable *table,uint32_t n) {
    CHECK(table && n<table->count && (!!table->declarations != !!table->identities));
    if(table->identities)return table->identities[n];
    const XrXirNominalDeclaration *d=table->declarations+n;
    return (XrXirNominalIdentity){.module=d->module,.name=d->name,.field_count=d->field_count,
        .arity=d->parameter_count,.kind=d->kind,.parent=d->parent,.exported=d->exported};
}
static XrXirNominalFieldIdentity class_inheritance_field(const XrXirNominalTable *table,uint32_t n,uint32_t f) {
    XrXirNominalIdentity header=class_inheritance_header(table,n);CHECK(f<header.field_count);
    if(table->identities)return table->identities[n].fields[f];
    const XrXirNominalField *field=table->declarations[n].fields+f;
    return (XrXirNominalFieldIdentity){field->name,field->flags};
}
static uint32_t class_inheritance_nominal(const XrXirNominalTable *table,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t n=0;n<table->count;++n) {
        XrXirNominalIdentity header=class_inheritance_header(table,n);
        if(header.name.length==strlen(name) && !memcmp(header.name.bytes,name,strlen(name))) {
            CHECK(found==UINT32_MAX);found=n;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static uint32_t class_inheritance_module(const XrXirModule *module,XrXirLiteral name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t m=0;m<module->declarations->module_count;++m) {
        const XrXirSourceModule *row=module->declarations->modules+m;
        if(row->name_length==name.length && !memcmp(row->name,name.bytes,name.length)) {CHECK(found==UINT32_MAX);found=m;}
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void class_inheritance_plan(const XrXirModule *module,unsigned mode,ClassInheritancePlan *plan) {
    memset(plan,0,sizeof(*plan));plan->entry=module->declarations->entry_function;
    CHECK(plan->entry<module->function_count && module->functions[plan->entry].result==XR_XIR_I64);
    if(!mode)return;
    bool answer=false,static_caller=false;uint32_t references=0;
    const char *target=mode==2?"runHidden":"answer";
    const XrXirNominalTable *table=module->types->nominals;CHECK(table);
    plan->dog=class_inheritance_nominal(table,"Dog");plan->animal=class_inheritance_nominal(table,"Animal");
    XrXirNominalIdentity dog=class_inheritance_header(table,plan->dog),animal=class_inheritance_header(table,plan->animal);
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=module->functions+f;
        if(body->name_length==strlen(target) && !memcmp(body->name,target,strlen(target))) {CHECK(!answer);answer=true;plan->entry=f;}
        for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_CLASS_METHOD_REF) {
            ++references;
            if(mode==2 && body->name_length==8 && !memcmp(body->name,"retained",8)) {
                uint32_t caller=module->declarations->functions[f].module;
                CHECK(xr_xir_module_imports(module->declarations,caller,class_inheritance_module(module,animal.module)));
                CHECK(!xr_xir_module_imports(module->declarations,caller,class_inheritance_module(module,dog.module)));
                CHECK(!dog.exported);static_caller=true;
            }
        }
    }
    CHECK(answer && references && (mode!=2 || static_caller));
    CHECK(dog.parent==plan->animal+1 && !animal.parent && dog.field_count==3 && animal.field_count==2);
    const XrXirTypeNode *dog_type=NULL,*animal_type=NULL;
    for(uint32_t t=0;t<module->types->count;++t) {
        const XrXirTypeNode *type=module->types->nodes+t;if(type->kind!=XR_XIR_TYPE_NOMINAL)continue;
        if(type->nominal.declaration==plan->dog) {
            CHECK(!dog_type);dog_type=type;plan->dog_type=XR_XIR_CONSTRUCTED_TYPE_BASE+t;
        }
        if(type->nominal.declaration==plan->animal) {CHECK(!animal_type);animal_type=type;}
    }
    CHECK(dog_type && animal_type && dog_type->nominal.field_count==3 && animal_type->nominal.field_count==2);
    for(uint32_t f=0;f<animal.field_count;++f) {
        XrXirNominalFieldIdentity left=class_inheritance_field(table,plan->dog,f);
        XrXirNominalFieldIdentity right=class_inheritance_field(table,plan->animal,f);
        CHECK(dog_type->nominal.fields[f]==animal_type->nominal.fields[f] && left.flags==right.flags);
        CHECK(left.name.length==right.name.length && !memcmp(left.name.bytes,right.name.bytes,left.name.length));
    }
    plan->answer=42;plan->lifetime=true;plan->hidden=mode==2;
}
static XrXirArtifact *class_inheritance_product(const XrXirCompileContext *context,unsigned mode,
    ClassInheritancePlan *plan) {
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *root=NULL;
    CHECK(xr_realpath_owned(&policy,mode?XR_CLASS_INHERITANCE_WORK:XR_CLASS_INHERITANCE_ROOT,&root)==XR_OS_IO_OK);
    XrXirLibraryCatalog *catalog=mode?class_inheritance_catalog(context,root):NULL;
    if(mode==2) {
        XrXirLibraryCatalog *hidden=class_inheritance_hidden_catalog(context,root,catalog);
        xr_xir_compile_library_catalog_free(catalog);catalog=hidden;
    }
    static const char hidden_root[]="import {callHidden} from \"./facade\"\n"
        "export fn runHidden()->i64{const value=callHidden();assert(value==42);print(\"hidden receiver passed\");return value}\n";
    ClassInheritanceSource fixture={mode==2?hidden_root:mode?class_inheritance_consumer:NULL,
        mode?"consumer.xr":"demos/03-oop/classes.xr",root,XR_XIR_PROGRAM,catalog,NULL};
    XrXirSourceResult source={0};CHECK(class_inheritance_check(context,&fixture,&source)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);xr_compile_resources_free(root);
    XrXirArtifact *lowered=class_inheritance_receive(context,&source);
    class_inheritance_plan(xr_xir_compile_artifact_module(lowered),mode,plan);return lowered;
}
static void class_inheritance_refusals(const XrXirCompileContext *context) {
    static const char *const texts[]={
        "final class Base{value:i64 init(value:i64){this.value=value}}\n"
        "class Child extends Base{init(value:i64){super(value)}}\n",
        "class Base{value:i64 init(value:i64){this.value=value} get()->i64{return this.value}}\n"
        "class Child extends Base{init(value:i64){super(value)} get()->i64{return this.value}}\n",
        "class Base implements Sendable{value:i64 init(value:i64){this.value=value}}\n"
        "class Child extends Base{init(value:i64){super(value)}}\n",
        "final class Counter{private count:i64 init(){this.count=3} getCount()->i64{return this.count}}\n"
        "fn bad(c:Counter)->i64{return c.count}\n"};
    for(unsigned kind=0;kind<sizeof(texts)/sizeof(*texts);++kind) {
        uint64_t before=class_inheritance_held(context);ClassInheritanceSource fixture={texts[kind],"refusal",NULL,XR_XIR_PROGRAM,NULL,NULL};
        XrXirSourceResult source={0};XrXirStatus status=class_inheritance_check(context,&fixture,&source);
        CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot);
        xr_xir_compile_source_result_free(&source);CHECK(class_inheritance_held(context)==before);
    }
}
typedef struct ClassInheritanceCancel {uint32_t polls,stop;} ClassInheritanceCancel;
static bool class_inheritance_cancel(const void *opaque) {
    ClassInheritanceCancel *state=(ClassInheritanceCancel *)opaque;return ++state->polls==state->stop;
}
static void class_inheritance_long_chain(const XrXirCompileContext *context) {
    uint64_t before=class_inheritance_held(context);char *text=NULL;size_t capacity=65536,used=0;
    CHECK(xr_compile_resources_alloc(context->resources,capacity,(void **)&text)==XR_COMPILE_RESOURCE_OK);
    int size=snprintf(text,capacity,"class C0{value:i64 init(value:i64){this.value=value}}\n");CHECK(size>0);used=(size_t)size;
    for(unsigned n=1;n<256;++n) {
        size=snprintf(text+used,capacity-used,"class C%u extends C%u{init(value:i64){super(value)}}\n",n,n-1);
        CHECK(size>0 && (size_t)size<capacity-used);used+=(size_t)size;
    }
    ClassInheritanceSource fixture={text,"long-chain",NULL,XR_XIR_PROGRAM,NULL,NULL};
    uint64_t request_before=class_inheritance_held(context);XrXirSourceResult source={0};
    CHECK(class_inheritance_check(context,&fixture,&source)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(class_inheritance_held(context)==request_before);
    ClassInheritanceCancel cancel={0,64};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={class_inheritance_cancel,&cancel,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);fixture.control=&control;
    CHECK(class_inheritance_check(context,&fixture,&source)==XR_XIR_CANCELLED && cancel.polls==64);
    CHECK(!source.checked && !source.snapshot && class_inheritance_held(context)==request_before);
    fixture.control=NULL;CHECK(class_inheritance_check(context,&fixture,&source)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_resources_free(text);CHECK(class_inheritance_held(context)==before);
}
static void class_inheritance_emit(const XrXirArtifact *code,const ClassInheritancePlan *plan,
    unsigned mode,const char *path) {
    char prefix[64];CHECK(snprintf(prefix,sizeof(prefix),"class_inheritance_%u",mode)>0);
    XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,prefix,16777216,&generated)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(generated.text,1,generated.length,file)==generated.length);
    CHECK(fprintf(file,"\nconst uint32_t class_inheritance_%u_metadata[]={%u,%u,%u,%u};\n",mode,
        plan->entry,plan->dog,plan->animal,plan->dog_type)>0 && !fclose(file));
    xr_xir_compile_c_source_free(&generated);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==5 && !strcmp(argv[1],"--emit")));class_inheritance_golden();
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    uint64_t before=class_inheritance_held(&context);class_inheritance_refusals(&context);class_inheritance_long_chain(&context);
    for(unsigned mode=0;mode<3;++mode) {
        ClassInheritancePlan plan={0};XrXirArtifact *code=class_inheritance_product(&context,mode,&plan);
        if(argc==5){class_inheritance_emit(code,&plan,mode,argv[2+mode]);xr_xir_compile_artifact_free(code);}
        else {
            XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
            class_inheritance_execute(program,&plan);xr_xir_compile_program_drop(program);
        }
        CHECK(class_inheritance_held(&context)==before);
    }
    xr_compile_resources_release(context.resources);CHECK(!cell_owner_live && !cell_owner_bytes);
    puts("Class inheritance original/cold receiver and physical retirement controls PASS");return 0;
}
