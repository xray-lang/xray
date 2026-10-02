/* Real captured bytes, independent expectations, and physical owner failures. */
#include "app/toolchain/xtc_dependencies.h"
#include "base/xmalloc.h"
#include "base/xjson_cursor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static void *fixture_allocate(size_t size) { return xr_malloc(size); }
static void fixture_release(void *memory) { xr_free(memory); }

#ifndef DEPENDENCY_PRODUCTION
typedef struct Block { void *p; size_t bytes; } Block;
static Block blocks[128];
static size_t allocation_calls, fail_at = SIZE_MAX, physical, total, peak;
static void *observed_malloc(size_t bytes) {
    if (allocation_calls++ == fail_at) return NULL;
    void *p = xr_malloc(bytes); CHECK(p);
    for (unsigned i = 0; i < 128; ++i) if (!blocks[i].p) {
        blocks[i] = (Block){p,bytes}; physical += bytes; total += bytes;
        if (physical > peak) peak = physical;
        return p;
    }
    CHECK(false); return NULL;
}
static void observed_free(void *p) {
    if (!p) return;
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].p == p) {
        physical -= blocks[i].bytes; blocks[i] = (Block){0}; xr_free(p); return;
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
#endif

static const XrCompileResourceLimits unlimited = {UINT64_MAX,UINT64_MAX,UINT64_MAX};
static const XrDependencyLimits shape = {1024 * 1024,32768,4096};
static const char *const arguments[] = {"C:\\tools\\compiler.exe", "-c", "C:\\src\\a.c"};
static size_t all_oom, all_work;
static void reset(void);
static XrJsonCursorStatus cursor_charge(void *context,uint64_t units) {
    return xr_compile_resources_work(context,units)==XR_COMPILE_RESOURCE_OK ? XR_JSON_CURSOR_OK : XR_JSON_CURSOR_BUDGET;
}
static void cursor_fixed_work(void) {
    reset();
    XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before,after; CHECK(xr_compile_resources_stats(r,&before)==XR_COMPILE_RESOURCE_OK);
    XrJsonCursor json=xr_json_cursor_make(NULL,0,r,cursor_charge);
    CHECK(xr_json_cursor_equal(&json,"a","a"));
    CHECK(xr_compile_resources_stats(r,&after)==XR_COMPILE_RESOURCE_OK && after.work-before.work==4);
    char source[]="\"\\u0061\""; const char *text=NULL;
    json=xr_json_cursor_make(source,8,r,cursor_charge); before=after;
    CHECK(xr_json_cursor_string(&json,1,&text) && !strcmp(text,"a"));
    CHECK(xr_compile_resources_stats(r,&after)==XR_COMPILE_RESOURCE_OK && after.work-before.work==11);
    xr_compile_resources_release(r);
    XrCompileResourceLimits budget=unlimited;budget.work=1;
    r=NULL;CHECK(xr_compile_resources_new(&budget,&r)==XR_COMPILE_RESOURCE_OK);
    json=xr_json_cursor_make(NULL,0,r,cursor_charge);
    CHECK(!xr_json_cursor_equal(&json,(const char *)(uintptr_t)1,(const char *)(uintptr_t)1));
    CHECK(json.status==XR_JSON_CURSOR_BUDGET);
    xr_compile_resources_release(r);
}
static char *read_file(const char *directory, const char *name, size_t *length) {
    char path[4096]; CHECK(snprintf(path,sizeof(path),"%s/%s",directory,name)>0);
    FILE *file = fopen(path,"rb"); CHECK(file);
    CHECK(!fseek(file,0,SEEK_END)); long size = ftell(file); CHECK(size >= 0);
    CHECK(!fseek(file,0,SEEK_SET)); char *text = fixture_allocate((size_t)size+1); CHECK(text);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size); CHECK(!fclose(file));
    text[size]=0; *length=(size_t)size; return text;
}
static void reset(void) {
#ifndef DEPENDENCY_PRODUCTION
    CHECK(!physical); allocation_calls=total=peak=0; fail_at=SIZE_MAX;
#endif
}
static void stats(XrCompileResources *r, XrCompileResourceStats *s) {
    CHECK(xr_compile_resources_stats(r,s)==XR_COMPILE_RESOURCE_OK);
#ifndef DEPENDENCY_PRODUCTION
    CHECK(s->live_bytes==physical && s->allocated_bytes==total && s->peak_bytes==peak);
#endif
}
static XrXirTargetStatus trial(const XrDependencyInput *input, XrDependencyLimits limits,
    const XrCompileResourceLimits *budget, XrCompileResourceStats *measured) {
    XrCompileResources *r=NULL; XrDependencyFacts *f=NULL;
    XrCompileResourceStatus rs=xr_compile_resources_new(budget,&r);
    if (rs!=XR_COMPILE_RESOURCE_OK) return rs==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_BUDGET;
    XrXirTargetStatus result=xtc_dependencies_parse(r,input,limits,&f);
    if (result==XR_XIR_TARGET_OK) {
        CHECK(f && xtc_dependencies_resources(f)==r && xtc_dependencies_status(f)==XR_XIR_TARGET_OK);
        CHECK(!xtc_dependencies_record(f,xtc_dependencies_count(f)));
        XrDependencyResponseFacts response=(XrDependencyResponseFacts)123;
        CHECK(xtc_dependencies_response_facts(f,&response)==XR_XIR_TARGET_OK && response==XR_DEPENDENCY_NO_REFERENCE_IN_CAPTURED_ARGV);
    } else CHECK(!f);
    stats(r,measured); xtc_dependencies_free(f);
    xr_compile_resources_release(r);
#ifndef DEPENDENCY_PRODUCTION
    CHECK(!physical);
#endif
    return result;
}
static void boundaries(const XrDependencyInput *input, bool every_work) {
    XrCompileResourceStats baseline={0}, used={0}; reset();
    CHECK(trial(input,shape,&unlimited,&baseline)==XR_XIR_TARGET_OK);
#ifndef DEPENDENCY_PRODUCTION
    size_t count=allocation_calls;
    for (size_t i=0;i<count;++i) {
        reset(); fail_at=i;
        CHECK(trial(input,shape,&unlimited,&used)==XR_XIR_TARGET_OUT_OF_MEMORY);
        CHECK(allocation_calls==i+1 && !physical); ++all_oom;
    }
#endif
    for (unsigned axis=0;axis<3;++axis) {
        uint64_t bound=axis==0 ? baseline.allocated_bytes : axis==1 ? baseline.peak_bytes : baseline.work;
        for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits budget=unlimited;
            if (axis==0) budget.allocated_bytes=bound-minus;
            if (axis==1) budget.live_bytes=bound-minus;
            if (axis==2) budget.work=bound-minus;
            reset(); CHECK(trial(input,shape,&budget,&used)==(minus ? XR_XIR_TARGET_BUDGET : XR_XIR_TARGET_OK));
        }
    }
    uint64_t step=every_work ? 1 : baseline.work/100+1;
    for (uint64_t cut=0;cut<baseline.work;cut+=step) {
        XrCompileResourceLimits budget=unlimited; budget.work=cut; reset();
        CHECK(trial(input,shape,&budget,&used)==XR_XIR_TARGET_BUDGET); ++all_work;
    }
    printf("format=%d work=%llu allocated=%llu peak=%llu work-cut-step=%llu\n",input->format,
        (unsigned long long)baseline.work,(unsigned long long)baseline.allocated_bytes,
        (unsigned long long)baseline.peak_bytes,(unsigned long long)step);
}
static void captured(const char *directory, const char *name, XrDependencyFormat format, uint32_t count) {
    char rawname[64], expectname[64];
    CHECK(snprintf(rawname,sizeof(rawname),"%s.%s",name,format==XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN ? "log" : "deps")>0);
    CHECK(snprintf(expectname,sizeof(expectname),"%s.expected",name)>0);
    size_t length, expected_length; char *bytes=read_file(directory,rawname,&length);
    char *expected=read_file(directory,expectname,&expected_length); (void)expected_length;
    XrDependencyInput input={format,(uint8_t *)bytes,length,arguments,3};
    reset(); XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    XrDependencyFacts *f=NULL; CHECK(xtc_dependencies_parse(r,&input,shape,&f)==XR_XIR_TARGET_OK);
    CHECK(xtc_dependencies_count(f)==count);
    memset(bytes,0xcd,length); fixture_release(bytes); xr_compile_resources_release(r);
    char *line=expected;
    for (uint32_t i=0;i<count;++i) {
        char *end; unsigned long kind=strtoul(line,&end,10); CHECK(*end++=='\t');
        unsigned long long offset=strtoull(end,&end,10); CHECK(*end++=='\t');
        char *next=strchr(end,'\n'); CHECK(next); *next=0;
        const XrDependencyRecord *record=xtc_dependencies_record(f,i); CHECK(record);
        CHECK(record->kind==(XrDependencyKind)kind && record->offset==(size_t)offset && !strcmp(record->path,end));
        line=next+1;
    }
    CHECK(!*line); fixture_release(expected);
    if (format==XR_DEPENDENCY_WINDOWS_MAKE) {
        CHECK(snprintf(expectname,sizeof(expectname),"%s.target",name)>0);
        expected=read_file(directory,expectname,&expected_length);
        CHECK(!strcmp(xtc_dependencies_target(f),expected)); fixture_release(expected);
    } else CHECK(!xtc_dependencies_target(f));
    xtc_dependencies_free(f);
    bytes=read_file(directory,rawname,&length); input.bytes=(uint8_t *)bytes;
    boundaries(&input,false); fixture_release(bytes);
}
static void expect(XrDependencyFormat format, const char *bytes, XrXirTargetStatus expected) {
    XrDependencyInput input={format,(const uint8_t *)bytes,strlen(bytes),arguments,3};
    XrCompileResourceStats used={0}; reset();
    XrXirTargetStatus actual=trial(&input,shape,&unlimited,&used);
    if(actual!=expected) fprintf(stderr,"expected %d got %d: %s\n",expected,actual,bytes);
    CHECK(actual==expected);
}
static void rejection_and_work(void) {
    static const char good[]="{\"Version\":\"1.2\",\"Data\":{\"Source\":\"C:/源/\\ud83d\\ude00.c\",\"ProvidedModule\":\"\",\"Includes\":[\"C:/a.h\",\"C:/b.h\",\"C:/c.h\",\"C:/d.h\",\"C:/e.h\",\"C:/f.h\",\"C:/g.h\",\"C:/h.h\",\"C:/i.h\"]}}";
    static const char make[]="C:\\out\\a.obj: \\\r\n C:\\src\\a.c \\\r\n C:\\has\\ space\\a.h\r\n";
    static const char link[]="正在搜索库\r\n    正在搜索 C:\\has space\\a.lib:\r\n已完成库搜索\r\n";
    const char *samples[]={good,make,link};
    for(unsigned i=0;i<3;++i) {
        XrDependencyInput input={(XrDependencyFormat)i,(const uint8_t *)samples[i],strlen(samples[i]),arguments,3};
        boundaries(&input,true);
    }
    expect(XR_DEPENDENCY_MSVC_SOURCE_1_2,"{\"Version\":\"1.3\",\"Data\":{}}",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_MSVC_SOURCE_1_2,"{\"Version\":\"1.2\",\"Version\":\"1.2\"}",XR_XIR_TARGET_INVALID);
    expect(XR_DEPENDENCY_MSVC_SOURCE_1_2,"{\"Unknown\":\"x\"}",XR_XIR_TARGET_INVALID);
    expect(XR_DEPENDENCY_MSVC_SOURCE_1_2,"{\"Version\":\"1.2\",\"Data\":{\"Source\":\"C:/a.c\",\"ProvidedModule\":\"mod\",\"Includes\":[]}}",XR_XIR_TARGET_UNSUPPORTED);
    const char *badpaths[]={"","relative.h","C:/\\ud800.h","C:/\\udc00.h","C:/\\ud800\\u0041.h","C:/\\u0000.h","C:/\\q.h","C:/\xc0\xaf.h"};
    for(unsigned i=0;i<sizeof(badpaths)/sizeof(*badpaths);++i) {
        char text[512]; CHECK(snprintf(text,sizeof(text),"{\"Version\":\"1.2\",\"Data\":{\"Source\":\"%s\",\"ProvidedModule\":\"\",\"Includes\":[]}}",badpaths[i])>0);
        expect(XR_DEPENDENCY_MSVC_SOURCE_1_2,text,XR_XIR_TARGET_INVALID);
    }
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a.c\r\nC:/b.obj: C:/b.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a#b.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a$$b.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a.c;echo\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/%.o: C:/a.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a*.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a?.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/[ab].c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj: C:/a.c|C:/b.h\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj&: C:/a.c\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_WINDOWS_MAKE,"C:/a.obj:\r\n",XR_XIR_TARGET_INVALID);
    expect(XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,"正在搜索库\r\n未知消息\r\n已完成库搜索\r\n",XR_XIR_TARGET_UNSUPPORTED);
    expect(XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,"正在搜索库\r\n",XR_XIR_TARGET_INVALID);
    expect(XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,"    正在搜索 C:/a.lib:\r\n",XR_XIR_TARGET_INVALID);
    expect(XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,"Searching libraries\r\n",XR_XIR_TARGET_UNSUPPORTED);
    const char *early[]={
        "{\"Version\":\"1.2\",\"Data\":{\"Source\":\"C:/\\u0001\",\"ProvidedModule\":\"\",\"Includes\":[]}}",
        "{\"Version\":\"1.2\",\"Data\":{\"Source\":\"C:/\\u0001abcdefghij\",\"ProvidedModule\":\"\",\"Includes\":[]}}"};
    XrCompileResourceStats early_stats[2];
    for (unsigned i=0;i<2;++i) {
        XrDependencyInput invalid={XR_DEPENDENCY_MSVC_SOURCE_1_2,(const uint8_t *)early[i],strlen(early[i]),arguments,3};
        reset();CHECK(trial(&invalid,shape,&unlimited,&early_stats[i])==XR_XIR_TARGET_INVALID);
    }
    /* Each suffix byte is copied once, decoded with one read/write, counted
     * once, and UTF-8 validated once. The invalid-character scan has stopped. */
    CHECK(early_stats[1].work-early_stats[0].work==5*10);
    XrDependencyInput input={XR_DEPENDENCY_MSVC_SOURCE_1_2,(const uint8_t *)good,strlen(good),arguments,3};
    for (size_t length=1;length<strlen(good);++length) {
        input.length=length; XrCompileResourceStats used; reset();
        CHECK(trial(&input,shape,&unlimited,&used)!=XR_XIR_TARGET_OK);
    }
    input.length=strlen(good);
    const char *response[]={"compiler", "C:/x@y"}; input.argv=response; input.argc=2;
    XrCompileResourceStats used; reset(); CHECK(trial(&input,shape,&unlimited,&used)==XR_XIR_TARGET_UNSUPPORTED);
    input.argv=arguments;input.argc=3;
    XrDependencyLimits small=shape;small.records=9;reset();CHECK(trial(&input,small,&unlimited,&used)==XR_XIR_TARGET_BUDGET);
    small=shape;small.frame_bytes=input.length-1;reset();CHECK(trial(&input,small,&unlimited,&used)==XR_XIR_TARGET_BUDGET);
    small=shape;small.path_bytes=3;reset();CHECK(trial(&input,small,&unlimited,&used)==XR_XIR_TARGET_BUDGET);
    static const uint8_t nul_make[]="C:/a.obj: C:/a.c\0hidden.h\r\n";
    XrDependencyInput malformed={XR_DEPENDENCY_WINDOWS_MAKE,nul_make,sizeof(nul_make)-1,arguments,3};
    reset(); CHECK(trial(&malformed,shape,&unlimited,&used)==XR_XIR_TARGET_INVALID);
    static const char malformed_make[]="C:/a.obj: C:/\xf4\x90\x80\x80.c\r\n";
    expect(XR_DEPENDENCY_WINDOWS_MAKE,malformed_make,XR_XIR_TARGET_INVALID);
    for (unsigned i=1;i<3;++i) {
        malformed=(XrDependencyInput){(XrDependencyFormat)i,(const uint8_t *)samples[i],strlen(samples[i]),arguments,3};
        for (size_t n=1;n<strlen(samples[i]);++n) {
            malformed.length=n; reset();
            XrXirTargetStatus status=trial(&malformed,shape,&unlimited,&used);
            /* A Make prefix ending in a complete absolute prerequisite, and
             * a complete final diagnostic marker without its CRLF, are valid
             * shorter reports. No truncation/completeness authority is inferred. */
            CHECK(status==XR_XIR_TARGET_OK || status==XR_XIR_TARGET_INVALID || status==XR_XIR_TARGET_UNSUPPORTED);
        }
    }
    reset(); XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    XrDependencyFacts *canary=(XrDependencyFacts *)(uintptr_t)1;
    CHECK(xtc_dependencies_parse(r,&input,shape,&canary)==XR_XIR_TARGET_INVALID && canary==(XrDependencyFacts *)(uintptr_t)1);
    xr_compile_resources_release(r);
    /* Reusing the original owner never refreshes cumulative allocation. */
    reset(); XrCompileResourceStats baseline;
    CHECK(trial(&input,shape,&unlimited,&baseline)==XR_XIR_TARGET_OK);
    XrCompileResourceLimits once=unlimited; once.allocated_bytes=baseline.allocated_bytes;
    reset();r=NULL;CHECK(xr_compile_resources_new(&once,&r)==XR_COMPILE_RESOURCE_OK);
    XrDependencyFacts *first=NULL,*second=NULL;
    CHECK(xtc_dependencies_parse(r,&input,shape,&first)==XR_XIR_TARGET_OK);
    xtc_dependencies_free(first);
    CHECK(xtc_dependencies_parse(r,&input,shape,&second)==XR_XIR_TARGET_BUDGET && !second);
    xr_compile_resources_release(r);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    cursor_fixed_work();
    captured(argv[1],"msvc",XR_DEPENDENCY_MSVC_SOURCE_1_2,229);
    captured(argv[1],"clang",XR_DEPENDENCY_WINDOWS_MAKE,182);
    captured(argv[1],"zig",XR_DEPENDENCY_WINDOWS_MAKE,182);
    captured(argv[1],"msvc-link",XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,35);
    rejection_and_work(); reset();
    printf("dependency facts passed; actual OOM=%zu; work cutoffs=%zu\n",all_oom,all_work);
    return 0;
}
