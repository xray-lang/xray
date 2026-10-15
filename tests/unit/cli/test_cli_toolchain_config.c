/* Owned preference snapshots, physical allocation cuts and shared limits. */
#include "app/cli/xcli_toolchain_config.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static void *physical_malloc(size_t size) { return xr_malloc(size); }
static void physical_free(void *memory) { xr_free(memory); }
typedef struct Block { void *pointer; size_t size; } Block;
static Block blocks[4096];
static size_t calls, fail_at = SIZE_MAX, physical_live;
static void *observed_malloc(size_t size) {
    if (calls++ == fail_at) return NULL;
    void *memory = physical_malloc(size);
    if (!memory) return NULL;
    for (size_t i=0;i<4096;++i) if (!blocks[i].pointer) {
        blocks[i]=(Block){memory,size}; physical_live+=size; return memory;
    }
    CHECK(false); return NULL;
}
static void observed_free(void *memory) {
    if (!memory) return;
    for (size_t i=0;i<4096;++i) if (blocks[i].pointer==memory) {
        physical_live-=blocks[i].size; blocks[i]=(Block){0}; physical_free(memory); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
static const XrCompileResourceLimits defaults={UINT64_C(1)<<28,UINT64_C(1)<<26,UINT64_C(1)<<32};
static XrCliToolchainConfigStatus run(const char *path,size_t fault,
    const XrCompileResourceLimits *limits,XrCompileResourceStats *stats,bool check) {
    CHECK(!physical_live); calls=0; fail_at=fault;
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus allocation=xr_compile_resources_new(limits,&resources);
    if (allocation!=XR_COMPILE_RESOURCE_OK) { CHECK(!physical_live);return XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY; }
    XrCliToolchainConfig *owner=NULL;
    XrCliToolchainConfigStatus status=xr_cli_toolchain_config_open(resources,path,&owner);
    CHECK((status==XR_CLI_TOOLCHAIN_CONFIG_OK)==(owner!=NULL));
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    if (status==XR_CLI_TOOLCHAIN_CONFIG_OK) {
        const XrCliToolchainConfigView *view=xr_cli_toolchain_config_view(owner);
        CHECK(view && view->path && view->preferences);
        XrCliToolchainConfig *occupied=owner;
        CHECK(xr_cli_toolchain_config_open(resources,path,&occupied)==XR_CLI_TOOLCHAIN_CONFIG_INVALID);
        CHECK(occupied==owner);
        if(check) {
            CHECK(view->exists && view->count==2 && !view->allow_fallback && view->auto_probe);
            CHECK(!strcmp(view->preferences[0].target,"native"));
            CHECK(view->preferences[0].selector==XR_TOOLCHAIN_SELECTOR_MSVC);
            CHECK(!strcmp(view->preferences[0].compiler,"C:/owned/编译器/cl.exe"));
            CHECK(!strcmp(view->preferences[1].target,"aarch64-linux-gnu"));
            CHECK(view->preferences[1].selector==XR_TOOLCHAIN_SELECTOR_ZIG);
        }
        xr_compile_resources_release(resources); resources=NULL;
        CHECK(xr_cli_toolchain_config_view(owner)==view && view->path[0]);
    } else {
        CHECK(!owner);
        XrCompileResourceStats before=*stats, after;
        if(status==XR_CLI_TOOLCHAIN_CONFIG_BUDGET || status==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY) {
            /* No snapshot survived, so another open is a new operation on the
             * same cumulative ledger. A single injected OOM is recoverable. */
            XrCliToolchainConfigStatus retry=xr_cli_toolchain_config_open(resources,path,&owner);
            CHECK(retry==(status==XR_CLI_TOOLCHAIN_CONFIG_BUDGET ?
                XR_CLI_TOOLCHAIN_CONFIG_BUDGET : XR_CLI_TOOLCHAIN_CONFIG_OK));
            CHECK((retry==XR_CLI_TOOLCHAIN_CONFIG_OK)==(owner!=NULL));
            CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>=before.work);
            CHECK(after.allocated_bytes<=limits->allocated_bytes && after.peak_bytes<=limits->live_bytes &&
                after.work<=limits->work);
            if(retry==XR_CLI_TOOLCHAIN_CONFIG_OK)
                CHECK(after.allocated_bytes>before.allocated_bytes && after.work>before.work);
        }
    }
    xr_cli_toolchain_config_free(owner);xr_compile_resources_release(resources);
    CHECK(!physical_live);return status;
}
static int test_main(int argc,char **argv) {
    CHECK(argc==3);
    XrCompileResourceStats stats={0};
    if(!strcmp(argv[2],"faults")) {
        CHECK(run(argv[1],SIZE_MAX,&defaults,&stats,true)==XR_CLI_TOOLCHAIN_CONFIG_OK);
        size_t count=calls;
        for(size_t cut=0;cut<count;++cut) {
            XrCompileResourceStats unused={0};
            CHECK(run(argv[1],cut,&defaults,&unused,false)==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY);
        }
        const uint64_t bounds[3]={stats.allocated_bytes,stats.peak_bytes,stats.work};
        for(unsigned axis=0;axis<3;++axis) for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits=defaults;
            uint64_t bound=bounds[axis];CHECK(bound>0);
            if(axis==0)limits.allocated_bytes=delta<0?bound-1:bound+(unsigned)delta;
            if(axis==1)limits.live_bytes=delta<0?bound-1:bound+(unsigned)delta;
            if(axis==2)limits.work=delta<0?bound-1:bound+(unsigned)delta;
            XrCompileResourceStats unused={0};
            XrCliToolchainConfigStatus status=run(argv[1],SIZE_MAX,&limits,&unused,delta>=0);
            CHECK(status==(delta<0?XR_CLI_TOOLCHAIN_CONFIG_BUDGET:XR_CLI_TOOLCHAIN_CONFIG_OK));
        }
        printf("complete allocation cuts=%zu axes=3 physical=0\n",count);return 0;
    }
    int expected=atoi(argv[2]);
    XrCliToolchainConfigStatus actual=run(argv[1],SIZE_MAX,&defaults,&stats,false);
    CHECK(actual==expected);return 0;
}
#ifdef XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=test_main(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
#else
int main(int argc,char **argv) { return test_main(argc,argv); }
#endif
