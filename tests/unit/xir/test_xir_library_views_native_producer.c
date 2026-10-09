/* Actual multi-module Source -> Checked -> specialization -> Lowered -> C. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_program.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"views producer %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_library_views_native_oracles.h"
static uint32_t h1n_export(const XrXirModule *module) {
    CHECK(module->declarations && module->declarations->functions);
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        const XrXirFunctionIdentity *id=&module->declarations->functions[f];
        if(fn->name_length!=6 || memcmp(fn->name,"result",6) ||
           id->module!=module->declarations->root_module || id->nominal_owner)continue;
        CHECK(found==UINT32_MAX && id->exported && !fn->parameter_count && fn->result==XR_XIR_I64);
        found=f;
    }
    CHECK(found!=UINT32_MAX);return found;
}
typedef struct H1ProducerFixture { uint32_t entry,run,module,slots; } H1ProducerFixture;
enum { H1C_OWNER, H1C_IO, H1C_SESSION, H1C_SOURCE, H1C_WRITE, H1C_READ,
    H1C_SPECIALIZE, H1C_CHECK, H1C_LOWER, H1C_LOWER_CHECK, H1C_SEAL, H1C_PHASES };
static const char *const h1c_names[H1C_PHASES] = {"owner", "source_io", "session", "Source", "Checked_write",
    "Checked_read", "specialize", "closed_reverify", "Lower", "Lowered_reverify", "CGen_W1_W4_publish"};
static const XrCompileResourceLimits h1c_caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
typedef struct H1Compiler {
    const RootParameterSourceOracle *oracle;
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrXirArtifact *checked, *read, *closed, *lowered;
    XrXirCheckedPacket packet;
    H1ProducerFixture fixture;
    XrXirCSource generated;
    XrCompileResourceStats baseline;
    size_t sites[H1C_PHASES], owner_blocks, owner_bytes;
    unsigned phase;
    char path[1024];
} H1Compiler;

static XrXirStatus h1c_io(XrOsIoStatus status);
static unsigned views_source_count(const H1Compiler *c) { return c->oracle->index==4 ? 18u : 6u; }
static void views_name(const H1Compiler *c,unsigned index,char *name,size_t capacity) {
    static const char *const names[]={"views_api.xr","views_left.xr","views_right.xr","views_leaf.xr","views_extra.xr","views_root.xr"};
    int n;
    if(c->oracle->index==4 && index<17)n=snprintf(name,capacity,"views_chain_%02u.xr",index);
    else n=snprintf(name,capacity,"%s",c->oracle->index==4 ? "views_root.xr" : names[index]);
    CHECK(n>0 && (size_t)n<capacity);
}
static void views_path(const H1Compiler *c,unsigned index,char *path,size_t capacity) {
    char name[64];views_name(c,index,name,sizeof(name));
    int n=snprintf(path,capacity,"%s/%s",XR_ROOT_PARAMETER_NATIVE_FIXTURES,name);
    CHECK(n>0 && (size_t)n<capacity);
}
static void h1c_host_clear(H1Compiler *c) {
    XrOsIoPolicy host=xr_os_io_system_policy();
    for(unsigned i=0;i<views_source_count(c);++i){
        char path[1024];views_path(c,i,path,sizeof(path));bool exists=false;
        XrOsIoStatus status=xr_os_io_exists(&host,path,&exists);
        CHECK(status==XR_OS_IO_OK || status==XR_OS_IO_NOT_FOUND);
        if(exists)CHECK(xr_os_io_remove(&host,path)==XR_OS_IO_OK);
    }
}
static XrXirStatus views_write_sources(H1Compiler *c) {
    static const char *const texts[]={
        "import \"./views_left\" as left;import \"./views_right\" as right;export fn answer()->i64{return left.answer()+right.answer();}\n",
        "import \"./views_leaf\" as leaf;export fn answer()->i64{return leaf.answer();}\n",
        "import \"./views_leaf\" as leaf;export fn answer()->i64{return leaf.answer()+1;}\n",
        "export fn answer()->i64{return 20;}\n",
        "export fn zero()->i64{return 0;}\n"};
    XrOsIoPolicy io=xr_compile_io_policy(c->context.resources);
    unsigned total=views_source_count(c);
    for(unsigned i=0;i<total;++i){
        char path[1024],chain[256];const char *text=NULL;views_path(c,i,path,sizeof(path));
        if(i==total-1){
            if(c->oracle->index==4)text="import \"./views_chain_00\" as chain;export fn result()->i64{return chain.answer();}\nprint(result())\n";
            else if(c->oracle->index==1)text="import \"./views_leaf\" as leaf;export fn result()->i64{return leaf.answer()+21;}\nprint(result())\n";
            else if(c->oracle->index>=2)text="import \"./views_api\" as api;import \"./views_extra\" as extra;export fn result()->i64{return api.answer()+extra.zero();}\nprint(result())\n";
            else text="import \"./views_api\" as api;export fn result()->i64{return api.answer();}\nprint(result())\n";
        } else if(c->oracle->index==4){
            if(i==16)text="export fn answer()->i64{return 41;}\n";
            else {int n=snprintf(chain,sizeof(chain),"import \"./views_chain_%02u\" as part;export fn answer()->i64{return part.answer();}\n",i+1);CHECK(n>0&&(size_t)n<sizeof(chain));text=chain;}
        } else text=texts[i];
        XrXirStatus status=h1c_io(xr_os_io_write_new_file_sync(&io,path,(const uint8_t *)text,strlen(text)));
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus h1c_io(XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return XR_XIR_OK;
    if (status == XR_OS_IO_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    if (status == XR_OS_IO_BUDGET) return XR_XIR_BUDGET;
    fprintf(stderr, "H1 producer unexpected IO=%u\n", (unsigned)status); CHECK(false); return XR_XIR_IO;
}
static uint64_t h1c_hash(const void *memory, size_t size) {
    const unsigned char *p = memory; uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static XrCompileResourceStats h1c_stats(H1Compiler *c) {
    XrCompileResourceStats s = {0};
    if (c->context.resources) CHECK(xr_compile_resources_stats(c->context.resources, &s) == XR_COMPILE_RESOURCE_OK);
    return s;
}
static void h1c_ledger(H1Compiler *c, const char *tag) {
    XrCompileResourceStats s = h1c_stats(c);
    printf("VIEWS_NATIVE_PRODUCER_COMPILER_LEDGER file=%s tag=%s owner=%p attempts=%zu allocated=%llu live=%llu peak=%llu work=%llu "
        "physical_blocks=%zu physical_bytes=%zu\n", c->oracle->file, tag, (void *)c->context.resources,
        instance_compile_attempts, (unsigned long long)s.allocated_bytes, (unsigned long long)s.live_bytes,
        (unsigned long long)s.peak_bytes, (unsigned long long)s.work, instance_compile_live, instance_compile_bytes);
}
static XrXirStatus views_session(H1Compiler *c) {
    XrCompilerSessionStatus status=xr_compile_session_new(c->context.resources,&c->session);
    if(status==XR_COMPILER_SESSION_OK)return XR_XIR_OK;
    CHECK(!c->session && (status==XR_COMPILER_SESSION_BUDGET || status==XR_COMPILER_SESSION_OUT_OF_MEMORY));
    return status==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static XrXirStatus views_source_request(H1Compiler *c,const char *path,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirArtifact **output) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_PARAMETER_NATIVE_FIXTURES};
    XrXirSourceRequest request={c->session,path,&authority,&c->context,NULL,NULL,kind,catalog};
    unsigned char saved[sizeof(request)];memcpy(saved,&request,sizeof(request));
    XrXirSourceResult result={0},empty={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    CHECK(!memcmp(saved,&request,sizeof(request)) && diagnostic.status==status);
    if(status==XR_XIR_OK){CHECK(result.checked&&result.snapshot&&!failure&&!*output);*output=result.checked;result.checked=NULL;}
    else CHECK(!memcmp(&result,&empty,sizeof(result)) && !*output);
    xr_compile_resources_free(failure);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(c->session);c->session=NULL;return status;
}
static XrXirStatus h1c_source(H1Compiler *c) {
    XrXirStatus status=XR_XIR_OK;
    XrXirArtifact *producer=NULL,*consumer=NULL;
    XrXirCheckedPacket packets[2]={{0},{0}};
    XrXirLibraryModuleInput bindings[2][17]={0};
    char names[2][17][64]={0};
    XrXirLibraryInput inputs[2]={0};XrXirLibraryCatalog *catalog=NULL;
    unsigned units=c->oracle->index==2 || c->oracle->index==3 ? 2u : 1u;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_PARAMETER_NATIVE_FIXTURES};
    for(unsigned u=0;u<units;++u){
        char path[1024];views_path(c,u ? 4u : 0u,path,sizeof(path));
        if(!c->session){status=views_session(c);if(status!=XR_XIR_OK)goto done;}
        status=views_source_request(c,path,XR_XIR_LIBRARY,NULL,&producer);if(status!=XR_XIR_OK)goto done;
        const XrXirModule *module=xr_xir_compile_artifact_module(producer);
        const XrXirDeclarations *d=module->declarations;
        CHECK(d&&d->module_count&&d->module_count<=17&&d->root_module==UINT32_MAX&&d->entry_function==UINT32_MAX&&!d->slot_count);
        CHECK(d->module_count==(u ? 1u : c->oracle->index==4 ? 17u : 4u));
        for(uint32_t m=0;m<d->module_count;++m){
            bool found=false;
            for(unsigned n=0;n<views_source_count(c)-1;++n){
                char name[64];views_name(c,n,name,sizeof(name));char *canonical=NULL;
                XrModuleStatus identity=xr_compile_module_identity_from_logical(c->context.resources,&authority,name,&canonical);
                if(identity!=XR_MODULE_OK){
                    CHECK(!canonical&&(identity==XR_MODULE_BUDGET||identity==XR_MODULE_OUT_OF_MEMORY));
                    status=identity==XR_MODULE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;
                }
                if(strlen(canonical)==d->modules[m].name_length&&!memcmp(canonical,d->modules[m].name,d->modules[m].name_length)){
                    CHECK(!found);size_t length=strlen(name);CHECK(length<sizeof(names[u][m]));
                    memcpy(names[u][m],name,length+1);bindings[u][m]=(XrXirLibraryModuleInput){authority,names[u][m]};found=true;
                }
                xr_compile_resources_free(canonical);
            }
            CHECK(found);
        }
        inputs[u].modules=bindings[u];inputs[u].module_count=d->module_count;
        status=xr_xir_compile_checked_write(producer,&packets[u],NULL);if(status!=XR_XIR_OK)goto done;
        xr_xir_compile_artifact_free(producer);producer=NULL;
        inputs[u].packet=packets[u].bytes;inputs[u].length=packets[u].length;
        xr_sha256(inputs[u].packet,inputs[u].length,inputs[u].sha256);
    }
    if(c->oracle->index==3){XrXirLibraryInput swap=inputs[0];inputs[0]=inputs[1];inputs[1]=swap;}
    status=xr_xir_compile_library_catalog_new_v2(&c->context,inputs,units,&catalog);if(status!=XR_XIR_OK)goto done;
    for(unsigned u=0;u<2;++u)xr_xir_compile_checked_packet_free(&packets[u]);
    memset(inputs,0,sizeof(inputs));memset(bindings,0,sizeof(bindings));memset(names,0,sizeof(names));
    /* The real Source consumer can now import only the owned Catalog views. */
    XrOsIoPolicy io=xr_compile_io_policy(c->context.resources);
    for(unsigned n=0;n<views_source_count(c)-1;++n){
        char path[1024];views_path(c,n,path,sizeof(path));status=h1c_io(xr_os_io_remove(&io,path));if(status!=XR_XIR_OK)goto done;
    }
    status=views_session(c);if(status!=XR_XIR_OK)goto done;
    status=views_source_request(c,c->path,XR_XIR_PROGRAM,catalog,&consumer);if(status!=XR_XIR_OK)goto done;
    CHECK(xr_xir_compile_artifact_module(consumer)->declarations->module_count==c->oracle->modules);
    status=h1c_io(xr_os_io_remove(&io,c->path));if(status!=XR_XIR_OK)goto done;
    c->checked=consumer;consumer=NULL;
done:
    xr_compile_session_free(c->session);c->session=NULL;
    xr_xir_compile_artifact_free(producer);xr_xir_compile_artifact_free(consumer);
    for(unsigned u=0;u<2;++u)xr_xir_compile_checked_packet_free(&packets[u]);
    xr_xir_compile_library_catalog_free(catalog);
    if(status==XR_XIR_OK)CHECK(c->checked);else CHECK(!c->checked);
    return status;
}

static XrXirStatus h1c_action(H1Compiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirStatus status = XR_XIR_OK; const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    switch (p) {
    case H1C_OWNER: {
        XrCompileResourceStatus s = xr_compile_resources_new(caps, &c->context.resources);
        if (s != XR_COMPILE_RESOURCE_OK) {
            CHECK(!c->context.resources && (s == XR_COMPILE_RESOURCE_BUDGET || s == XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
            return s == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        }
        c->context.limits = xr_xir_compile_default_limits(); c->baseline = h1c_stats(c);
        c->owner_blocks = instance_compile_live; c->owner_bytes = instance_compile_bytes;
        CHECK(c->owner_blocks == 1 && c->baseline.live_bytes == sizeof(*c->context.resources)); break;
    }
    case H1C_IO: return views_write_sources(c);
    case H1C_SESSION: {
        XrCompilerSessionStatus s = xr_compile_session_new(c->context.resources, &c->session);
        if (s != XR_COMPILER_SESSION_OK) {
            CHECK(!c->session && (s == XR_COMPILER_SESSION_BUDGET || s == XR_COMPILER_SESSION_OUT_OF_MEMORY));
            return s == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        }
        CHECK(xr_compile_session_resources(c->session) == c->context.resources); break;
    }
    case H1C_SOURCE: return h1c_source(c);
    case H1C_WRITE:
        status = xr_xir_compile_checked_write(c->checked, &c->packet, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->checked); c->checked = NULL; }
        else CHECK(!c->packet.bytes && !c->packet.length); break;
    case H1C_READ:
        status = xr_xir_compile_checked_read(&c->context, c->packet.bytes, c->packet.length, &c->read, NULL);
        if (status == XR_XIR_OK) { memset(c->packet.bytes, 0xa5, c->packet.length); xr_xir_compile_checked_packet_free(&c->packet); }
        else CHECK(!c->read); break;
    case H1C_SPECIALIZE:
        status = xr_xir_compile_specialize(c->read, &c->closed, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->read); c->read = NULL; }
        else CHECK(!c->closed); break;
    case H1C_CHECK: {
        status = xr_xir_compile_artifact_verify(c->closed, NULL); if (status != XR_XIR_OK) break;
        const XrXirModule *m = xr_xir_compile_artifact_module(c->closed);
        CHECK(m->stage == XR_XIR_CHECKED && m->provenance && m->provenance->kind == XR_XIR_EVIDENCE_INSTANCE);
        c->fixture.run = h1n_export(m); XrXirEffects *effects = NULL;
        status = xr_xir_compile_effects_analyze(c->closed, &effects);
        if (status == XR_XIR_OK) {
            const XrXirRootEffects *f = xr_xir_effects_root(effects, c->fixture.run);
            CHECK(f && f->requires_root == c->oracle->root && f->unresolved == c->oracle->unresolved);
        } else CHECK(!effects);
        xr_xir_compile_effects_free(effects); break;
    }
    case H1C_LOWER:
        status = xr_xir_compile_lower(c->closed, &target, &c->lowered, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->closed); c->closed = NULL; }
        else CHECK(!c->lowered); break;
    case H1C_LOWER_CHECK: {
        status = xr_xir_compile_artifact_verify(c->lowered, NULL); if (status != XR_XIR_OK) break;
        const XrXirModule *m = xr_xir_compile_artifact_module(c->lowered); const XrXirDeclarations *d = m->declarations;
        CHECK(m->stage == XR_XIR_LOWERED && h1n_export(m) == c->fixture.run && d->module_count == c->oracle->modules);
        CHECK(d->entry_function < m->function_count && d->entry_function != c->fixture.run &&
            !m->functions[d->entry_function].parameter_count && m->functions[d->entry_function].result == XR_XIR_I64);
        CHECK(d->slot_count == (c->oracle->root ? 1u : 0u));
        if (d->slot_count) CHECK(d->slots[0].module == d->root_module && d->slots[0].mutable && d->slots[0].type == XR_XIR_I64);
        c->fixture.entry = d->entry_function; c->fixture.module = d->root_module; c->fixture.slots = d->slot_count; break;
    }
    case H1C_SEAL: {
        char symbol[64]; CHECK(snprintf(symbol,sizeof(symbol),"h1native_%u",c->oracle->index)>0);
        status = xr_xir_compile_emit_c(c->lowered,symbol,4194304,&c->generated);
        if (status == XR_XIR_OK) {
            CHECK(c->generated.text && c->generated.length && !c->generated.text[c->generated.length]);
            CHECK(!strstr(c->generated.text,"({"));
            xr_xir_compile_artifact_free(c->lowered); c->lowered = NULL;
        } else CHECK(!c->generated.text && !c->generated.length);
        break; }
    default: CHECK(false); break;
    }
    return status;
}
static XrXirStatus h1c_step(H1Compiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirArtifact *input = p == H1C_WRITE ? c->checked : p == H1C_SPECIALIZE ? c->read :
        p == H1C_CHECK || p == H1C_LOWER ? c->closed : p >= H1C_LOWER_CHECK ? c->lowered : NULL;
    uint64_t hash = input ? h1c_hash(xr_xir_compile_artifact_module(input), sizeof(XrXirModule)) : 0;
    uint64_t packet_hash = p == H1C_READ ? h1c_hash(c->packet.bytes, c->packet.length) : 0;
    size_t begin = instance_compile_attempts; XrXirStatus status = h1c_action(c, p, caps);
    CHECK(instance_compile_attempts >= begin); c->sites[p] = instance_compile_attempts - begin; c->phase = p;
    if (status != XR_XIR_OK && input) CHECK(h1c_hash(xr_xir_compile_artifact_module(input), sizeof(XrXirModule)) == hash);
    if (status != XR_XIR_OK && p == H1C_READ) CHECK(h1c_hash(c->packet.bytes, c->packet.length) == packet_hash);
    printf("VIEWS_NATIVE_PRODUCER_COMPILER_STAGE file=%s phase=%u name=%s first=%zu end=%zu sites=%zu status=%u\n",
        c->oracle->file, p, h1c_names[p], begin, instance_compile_attempts, c->sites[p], (unsigned)status);
    h1c_ledger(c, h1c_names[p]); return status;
}
static void h1c_init(H1Compiler *c, const RootParameterSourceOracle *oracle) {
    *c = (H1Compiler){0}; c->oracle = oracle;
    int n = snprintf(c->path, sizeof(c->path), "%s/views_root.xr", XR_ROOT_PARAMETER_NATIVE_FIXTURES);
    CHECK(n > 0 && (size_t)n < sizeof(c->path)); h1c_host_clear(c);
}
static void h1c_clear(H1Compiler *c) {
    XrCompileResourceStats before = h1c_stats(c);
    xr_xir_compile_c_source_free(&c->generated);
    xr_compile_session_free(c->session); c->session = NULL;
    xr_xir_compile_artifact_free(c->checked); c->checked = NULL;
    xr_xir_compile_artifact_free(c->read); c->read = NULL;
    xr_xir_compile_artifact_free(c->closed); c->closed = NULL;
    xr_xir_compile_artifact_free(c->lowered); c->lowered = NULL;
    xr_xir_compile_checked_packet_free(&c->packet); h1c_host_clear(c);
    XrCompileResourceStats after = h1c_stats(c);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.work == before.work && after.peak_bytes == before.peak_bytes);
    if (c->context.resources) {
        CHECK(after.live_bytes == c->baseline.live_bytes && instance_compile_live == c->owner_blocks && instance_compile_bytes == c->owner_bytes);
    } else instance_compile_zero();
    h1c_ledger(c, "partial_free_fee_preserved");
}
static void h1c_drop(H1Compiler *c) {
    h1c_clear(c); xr_compile_resources_release(c->context.resources); c->context.resources = NULL;
    instance_compile_zero();
}
static XrXirStatus h1c_pipeline(H1Compiler *c, unsigned first, const XrCompileResourceLimits *caps) {
    for (unsigned p = first; p < H1C_PHASES; ++p) {
        XrXirStatus status = h1c_step(c, p, caps); if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static uint64_t h1_decimal(const char *text) {
    CHECK(text && *text); uint64_t value = 0;
    for (const char *p = text; *p; ++p) {
        CHECK(*p >= '0' && *p <= '9'); uint64_t digit = (uint64_t)(*p - '0');
        CHECK(value <= (UINT64_MAX - digit) / 10); value = value * 10 + digit;
    }
    return value;
}
static uint64_t h1_number(FILE *file) {
    char text[32] = {0}; CHECK(fscanf(file, "%31s", text) == 1); return h1_decimal(text);
}
static void h1_token(FILE *file, const char *expected) {
    char text[64] = {0}; CHECK(fscanf(file, "%63s", text) == 1 && !strcmp(text, expected));
}
static unsigned h1_unsigned(FILE *file) {
    uint64_t value = h1_number(file); CHECK(value <= UINT32_MAX); return (unsigned)value;
}
#define main views_resource_main
#include "xir_library_views_native_producer_main.h"
#undef main
int main(int argc,char **argv) {
    if(argc<2 || strcmp(argv[1],"--emit"))return views_resource_main(argc,argv);
    CHECK(argc==8);
    FILE *header=fopen(argv[7],"wb");CHECK(header);
    for(unsigned i=0;i<5;++i){
        H1Compiler c;h1c_init(&c,&rps_oracles[i]);
        CHECK(h1c_pipeline(&c,0,&h1c_caps)==XR_XIR_OK);
        CHECK(c.generated.text&&c.generated.length&&!c.generated.text[c.generated.length]);
        CHECK(!c.session&&!c.checked&&!c.read&&!c.closed&&!c.lowered&&!c.packet.bytes);
        CHECK(fprintf(header,"XR_DATA const XrXirProgramSpec h1native_%u_program;\nstatic const uint32_t h1native_%u_run = %uu;\n",i,i,c.fixture.run)>0);
        FILE *file=fopen(argv[i+2],"wb");CHECK(file);
        CHECK(fwrite(c.generated.text,1,c.generated.length,file)==c.generated.length&&!fclose(file));
        printf("VIEWS_GENERATED case=%s run=%u root=%u modules=%u bytes=%zu source_dead=1 catalog_dead=1 W1W4=1\n",
            c.oracle->file,c.fixture.run,c.fixture.module,c.oracle->modules,c.generated.length);
        h1c_drop(&c);
    }
    CHECK(!fclose(header));instance_compile_zero();return 0;
}
