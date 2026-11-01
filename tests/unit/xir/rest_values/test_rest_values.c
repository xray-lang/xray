/* Ordinary rest declarations, owned facts and cold receiving share real checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_source.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"rest %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include "../xir_source_program_compile_owner.h"
#include "../xir_runtime_allocations.h"
#include "rest_runtime.h"
static uint32_t rest_find(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==length &&
        !memcmp(module->functions[f].name,name,length)){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(resources,&s)==XR_COMPILE_RESOURCE_OK);return s.held_bytes;
}
static XrXirCompileContext context_new(void) {
    XrXirCompileContext c={.limits=xr_xir_compile_default_limits()};
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);return c;
}
static char *read_text(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>=0);
    CHECK(!fseek(file,0,SEEK_SET));char *text=xr_malloc((size_t)size+1);CHECK(text);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size&&!fclose(file));text[size]=0;return text;
}
static XrXirStatus source(const XrXirCompileContext *c,const char *text,XrXirSourceResult *out,
    XrCompileRequestControl *control) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(c->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"rest-values",NULL};
    const XrXirSourceRequest request={.session=session,.authority=&authority,.context=c,.linkage_kind=XR_XIR_PROGRAM};
    const XrXirSourceText input={.text=text,.length=strlen(text)};XrXirSourceDiagnostic diag={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,out,&diag,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"rest Source status%u %d:%d %s\n",status,diag.line,diag.column,diag.message);
    xr_compile_session_free(session);return status;
}
#include "rest_source_controls.h"
#include "rest_generator_controls.h"
extern void rest_producer_controls(XrXirArtifact *artifact);
void rest_producer_fail_next(void) { source_program_compile_injected=false;source_program_compile_fail_at=source_program_compile_attempts; }
void rest_producer_fault_clear(void) { source_program_compile_fail_at=SIZE_MAX; }
bool rest_producer_fault_observed(void) { return source_program_compile_injected; }
void rest_producer_physical(size_t *blocks,size_t *bytes) {
    *blocks=source_program_compile_live;*bytes=source_program_compile_bytes;
}
static void captured_factory_query(const XrXirSourceView *view) {
    uint32_t owner=0;bool found=false;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_FUNCTION&&d->name&&!strcmp(d->name,"capturedValues"))owner=d->id;
    }
    CHECK(owner);
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        static const char prefix[]="$generator_factory";
        if(d->kind!=XR_XIR_SOURCE_FUNCTION||!d->name||strncmp(d->name,prefix,sizeof(prefix)-1)||d->generic_parent!=owner)continue;
        CHECK(d->id!=owner&&d->generic_parameter_count==1&&d->generic_parent_count==1);
        CHECK(d->type.known&&d->type.generic_owner==owner);
        bool parameter=false;
        for(uint32_t p=0;p<d->parameter_count;++p)parameter|=d->parameters[p].known&&d->parameters[p].generic_owner==owner;
        CHECK(parameter);found=true;
    }
    CHECK(found);
}
static void query_controls(const XrXirCompileContext *c,XrXirSourceResult *r) {
    const XrXirSourceView *v=xr_xir_compile_source_snapshot_view(r->snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(r->snapshot);
    const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(r->snapshot);
    CHECK(v&&syntax&&calls&&v->declaration_count<256&&calls->count<256);
    XrXirSourceDeclaration declarations[256];memcpy(declarations,v->declarations,v->declaration_count*sizeof(*declarations));
    uint32_t rest=UINT32_MAX,indirect=UINT32_MAX;
    for(uint32_t i=0;i<v->declaration_count;++i)if(declarations[i].flags&XR_XIR_SOURCE_DECL_REST) {
        CHECK(declarations[i].kind==XR_XIR_SOURCE_FUNCTION&&declarations[i].parameter_count);
        CHECK(xr_xir_type_is_array(v->types,declarations[i].parameters[declarations[i].parameter_count-1].type));rest=i;
    }
    for(uint32_t i=0;i<calls->count;++i) {
        const XrXirSourceCallSignature *signature=&calls->items[i].signature;
        const XrXirTypeNode *callable=signature->callable.known ?
            xr_xir_callable_signature(v->types,signature->callable.type) : NULL;
        if(calls->items[i].reference==UINT32_MAX && signature->known && callable &&
            (callable->flags&XR_XIR_CALLABLE_REST))indirect=i;
    }
    CHECK(rest!=UINT32_MAX&&indirect!=UINT32_MAX);uint64_t before=held(c->resources);
    for(unsigned attack=0;attack<2;++attack) {
        XrXirSourceType parameters[64];uint32_t count=declarations[rest].parameter_count;CHECK(count&&count<=64);
        memcpy(parameters,declarations[rest].parameters,count*sizeof(*parameters));
        XrXirSourceDeclaration old=declarations[rest];declarations[rest].parameters=parameters;
        if(attack)parameters[count-1].known=false;else parameters[count-1].type=XR_XIR_I64;
        XrXirSourceView changed=*v;changed.declarations=declarations;XrXirSourceSnapshot *refused=NULL;
        CHECK(xr_xir_compile_source_snapshot_copy_v2(c,&changed,xr_xir_compile_source_snapshot_construction(r->snapshot),&refused)==XR_XIR_BAD_TYPE&&!refused);
        declarations[rest]=old;CHECK(held(c->resources)==before);
    }
    uint32_t generic=UINT32_MAX,rest_generic=UINT32_MAX,foreign=UINT32_MAX;
    for(uint32_t i=0;i<v->declaration_count;++i) {
        if(declarations[i].kind==XR_XIR_SOURCE_BINDING)foreign=i;
        if(declarations[i].flags&XR_XIR_SOURCE_DECL_REST) {
            const XrXirSourceType *tail=&declarations[i].parameters[declarations[i].parameter_count-1];
            if(tail->generic_owner){generic=tail->generic_owner-1;rest_generic=i;}
        }
    }
    CHECK(generic!=UINT32_MAX&&rest_generic!=UINT32_MAX&&foreign!=UINT32_MAX);
    for(unsigned attack=0;attack<2;++attack) {
        XrXirSourceDeclaration old=declarations[generic],original=declarations[rest_generic],fake=declarations[foreign];
        XrXirSourceType parameters[64];CHECK(original.parameter_count<=64);
        memcpy(parameters,original.parameters,original.parameter_count*sizeof(*parameters));
        if(attack)declarations[generic].generic_parameter_count=0;
        else {
            parameters[original.parameter_count-1].generic_owner=foreign+1;
            declarations[foreign].generic_parameter_count=old.generic_parameter_count;
            declarations[rest_generic].parameters=parameters;
        }
        XrXirSourceView changed=*v;changed.declarations=declarations;XrXirSourceSnapshot *refused=NULL;
        CHECK(xr_xir_compile_source_snapshot_copy_v2(c,&changed,xr_xir_compile_source_snapshot_construction(r->snapshot),&refused)==XR_XIR_BAD_TYPE&&!refused);
        declarations[generic]=old;declarations[rest_generic]=original;declarations[foreign]=fake;CHECK(held(c->resources)==before);
    }
    const size_t failures[]={0,3};
    for(unsigned i=0;i<2;++i) {
        XrXirSourceSnapshot *refused=NULL;source_program_compile_injected=false;
        source_program_compile_fail_at=source_program_compile_attempts+failures[i];
        XrXirStatus status=xr_xir_compile_source_snapshot_copy_v2(c,v,xr_xir_compile_source_snapshot_construction(r->snapshot),&refused);
        source_program_compile_fail_at=SIZE_MAX;
        CHECK(source_program_compile_injected&&status==XR_XIR_OUT_OF_MEMORY&&!refused&&held(c->resources)==before);
    }
    XrXirSourceCall items[256];memcpy(items,calls->items,calls->count*sizeof(*items));
    for(unsigned attack=0;attack<2;++attack) {
        XrXirSourceSnapshot *copy=NULL;
        CHECK(xr_xir_compile_source_snapshot_copy_v2(c,v,xr_xir_compile_source_snapshot_construction(r->snapshot),&copy)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(copy,syntax)==XR_XIR_OK);
        XrXirSourceCalls changed=*calls;changed.items=items;XrXirSourceCall old=items[indirect];
        if(attack)items[indirect].signature.parameter_flags=NULL;else items[indirect].signature.result.known=false;
        CHECK(xr_xir_compile_source_snapshot_calls_copy(copy,&changed)!=XR_XIR_OK);
        items[indirect]=old;xr_xir_compile_source_snapshot_free(copy);CHECK(held(c->resources)==before);
    }
}
static void raw_controls(XrXirArtifact *artifact) {
    const XrXirModule *m=&artifact->module;CHECK(m->function_count<128&&m->types->count<512);
    XrXirFunctionIdentity ids[128];XrXirFunction functions[128];XrXirTypeNode nodes[512];
    uint32_t sum=rest_find(m,"sum"),plain=rest_find(m,"ordinary");
    CHECK(m->declarations->functions[sum].promises&XR_XIR_FUNCTION_REST);
    for(unsigned attack=0;attack<4;++attack) {
        memcpy(ids,m->declarations->functions,m->function_count*sizeof(*ids));
        memcpy(functions,m->functions,m->function_count*sizeof(*functions));
        memcpy(nodes,m->types->nodes,m->types->count*sizeof(*nodes));
        XrXirDeclarations d=*m->declarations;d.functions=ids;XrXirTypes types=*m->types;types.nodes=nodes;
        XrXirArtifact invalid=*artifact;invalid.module.declarations=&d;invalid.module.functions=functions;invalid.module.types=&types;
        XrXirType bad=XR_XIR_I64;
        if(attack==0)ids[sum].promises&=~XR_XIR_FUNCTION_REST;
        if(attack==1)ids[plain].promises|=XR_XIR_FUNCTION_REST;
        if(attack==2)functions[sum].parameters=&bad;
        if(attack==3)for(uint32_t n=0;n<types.count;++n)if(nodes[n].kind==XR_XIR_TYPE_CALLABLE)nodes[n].flags^=XR_XIR_CALLABLE_REST;
        CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)!=XR_XIR_OK);
    }
    uint32_t prefixed=rest_find(m,"prefixed");CHECK(m->defaults&&m->defaults->count<=32);
    for(unsigned attack=0;attack<2;++attack) {
        XrXirDefaultBinding records[32];memcpy(records,m->defaults->records,m->defaults->count*sizeof(*records));
        memcpy(ids,m->declarations->functions,m->function_count*sizeof(*ids));
        bool changed=false;
        for(uint32_t i=0;i<m->defaults->count;++i)if(records[i].owner==prefixed) {
            if(attack)ids[prefixed].promises&=~XR_XIR_FUNCTION_REST;
            else records[i].ordinal=m->functions[prefixed].parameter_count-1;
            changed=true;
        }
        CHECK(changed);XrXirDefaultTable defaults={records,m->defaults->count};
        XrXirDeclarations declarations=*m->declarations;declarations.functions=ids;
        XrXirArtifact invalid=*artifact;invalid.module.defaults=&defaults;invalid.module.declarations=&declarations;
        CHECK(xr_xir_compile_defaults_verify(&artifact->context,&invalid.module)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)!=XR_XIR_OK);
    }
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
}
static XrXirArtifact *receive(XrXirCompileContext *c,XrXirSourceResult *r) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(r->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(r);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(c,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
#include "rest_cold.h"
static void execute(XrXirCompileContext *c,XrXirArtifact *lowered,const char *emit) {
    rest_runtime_refusals=0;
    const XrXirModule *m=&lowered->module;uint32_t entry=m->declarations->entry_function;
    answer_entry=rest_find(m,"referenceAnswer");rest_entry=rest_find(m,"sum");plain_entry=rest_find(m,"ordinary");
    rest_type=plain_type=XR_XIR_UNIT;
    for(uint32_t n=0;n<m->types->count;++n) {
        const XrXirTypeNode *t=&m->types->nodes[n];
        if(t->kind!=XR_XIR_TYPE_CALLABLE||t->parameter_count!=1||t->result!=XR_XIR_I64||
            !xr_xir_type_is_array(m->types,t->parameters[0].type)||xr_xir_array_element(m->types,t->parameters[0].type)!=XR_XIR_I64||
            (t->flags&XR_XIR_CALLABLE_ROOT_MASK)!=XR_XIR_CALLABLE_ROOT_NONE)continue;
        if(t->flags&XR_XIR_CALLABLE_REST)rest_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);
        else plain_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);
    }
    CHECK(rest_type&&plain_type);
    if(emit) {
        XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"rest_values",1048576,&output)==XR_XIR_OK);
        FILE *file=fopen(emit,"wb");CHECK(file&&fwrite(output.text,1,output.length,file)==output.length);
        CHECK(fprintf(file,"\nconst uint32_t rest_ids[5]={%u,%u,%u,%u,%u};\n",entry,answer_entry,rest_entry,plain_entry,(uint32_t)rest_type)>0);
        CHECK(fprintf(file,"const uint32_t rest_plain_type=%u;\n",(uint32_t)plain_type)>0&&!fclose(file));
        xr_xir_compile_c_source_free(&output);return;
    }
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK&&m->function_count<=256);
    for(uint32_t f=0;f<m->function_count;++f){rest_entries[f]=entries[f];entries[f].resume=rest_delegate;}
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(lowered),entries,m->function_count,
        m->declarations,{0},m->types,xir_program_packet_borrow(lowered)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(c,&spec,&program)==XR_XIR_OK);
    rest_execute(program,entry,answer_entry);CHECK(rest_runtime_refusals==8);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2||(argc==3&&(!strcmp(argv[1],"--cold")||!strcmp(argv[1],"--source"))));
    XrXirCompileContext c=context_new();uint64_t before=held(c.resources);
    if(argc==3&&!strcmp(argv[1],"--source")) {
        char *text=read_text(argv[2]);XrXirSourceResult result={0};XrXirStatus status=source(&c,text,&result,NULL);
        xr_free(text);xr_xir_compile_source_result_free(&result);CHECK(held(c.resources)==before);
        xr_compile_resources_release(c.resources);source_program_owners_free();return status==XR_XIR_OK?0:1;
    }
    XrXirSourceResult r={0};XrXirArtifact *lowered=NULL;
    if(argc!=3) {
        char *text=read_text(XR_REST_VALUES);CHECK(source(&c,text,&r,NULL)==XR_XIR_OK);
        if(argc==1){rest_source_refusals(&c);rest_request_failures(&c,text);}
        xr_free(text);captured_factory_query(xr_xir_compile_source_snapshot_view(r.snapshot));
        query_controls(&c,&r);raw_controls(r.checked);rest_generator_controls(r.checked);
        if(argc==1)rest_producer_controls(r.checked);
        lowered=receive(&c,&r);
        execute(&c,lowered,argc==2?argv[1]:NULL);xr_xir_compile_artifact_free(lowered);
        CHECK(held(c.resources)==before);
    }
    if(argc!=2) {
        r=rest_cold_two_hops(&c);lowered=receive(&c,&r);
        execute(&c,lowered,argc==3?argv[2]:NULL);xr_xir_compile_artifact_free(lowered);
    }
    CHECK(held(c.resources)==before);xr_compile_resources_release(c.resources);
    CHECK(!runtime_live&&!runtime_bytes);source_program_owners_free();
    puts("Rest values Source/owned query/packet/physical calls and retirement PASS");return 0;
}
