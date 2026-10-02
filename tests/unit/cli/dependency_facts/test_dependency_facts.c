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
    *measured=(XrCompileResourceStats){0};
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
    uint64_t step=baseline.work/100+1;
    for (uint64_t cut=every_work ? baseline.work : 0;
        every_work ? cut!=0 : cut<baseline.work;) {
        XrCompileResourceLimits budget=unlimited; budget.work=cut; reset();
        if(every_work) --budget.work;
        CHECK(trial(input,shape,&budget,&used)==XR_XIR_TARGET_BUDGET); ++all_work;
        if(every_work) {CHECK(used.work<cut);cut=used.work;} else cut+=step;
    }
    printf("format=%d work=%llu allocated=%llu peak=%llu work-cut-step=%llu\n",input->format,
        (unsigned long long)baseline.work,(unsigned long long)baseline.allocated_bytes,
        (unsigned long long)baseline.peak_bytes,(unsigned long long)(every_work ? 0 : step));
}
static void captured(const char *directory, const char *name, XrDependencyFormat format, uint32_t count) {
    char rawname[64], expectname[64];
    CHECK(snprintf(rawname,sizeof(rawname),"%s.%s",name,format==XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN ? "log" :
        format==XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE ? "rsp" : "deps")>0);
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
    boundaries(&input,format==XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE);
    if(format==XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE) {
        for(size_t n=0;n<length;++n) {
            input.length=n;XrCompileResourceStats used;reset();
            /* Complete earlier lines form a valid shorter explicit list.
             * All other prefixes truncate the mandatory UTF-16 framing. */
            bool complete=n>=6&&!(n&1)&&bytes[n-4]=='\r'&&!bytes[n-3]&&bytes[n-2]=='\n'&&!bytes[n-1];
            CHECK(trial(&input,shape,&unlimited,&used)==(complete?XR_XIR_TARGET_OK:XR_XIR_TARGET_INVALID));
        }
    }
    fixture_release(bytes);
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
static size_t rsp_ascii(const char *text, uint8_t *bytes) {
    bytes[0]=0xff;bytes[1]=0xfe;size_t length=2;
    for(size_t i=0;text[i];++i) {bytes[length++]=(uint8_t)text[i];bytes[length++]=0;}
    return length;
}
static XrCompileResourceStats rsp_expect(const uint8_t *bytes,size_t length,
    XrDependencyLimits limits,XrXirTargetStatus expected) {
    XrDependencyInput input={XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,bytes,length,arguments,3};
    XrCompileResourceStats used={0};reset();
    XrXirTargetStatus actual=trial(&input,limits,&unlimited,&used);
    if(actual!=expected)fprintf(stderr,"RSP expected %d got %d, length=%zu\n",expected,actual,length);
    CHECK(actual==expected);return used;
}
static void rsp_rejections(const char *directory) {
    size_t length;char *old=read_file(directory,"msvc-defaultlib.rsp",&length);
    rsp_expect((const uint8_t *)old,length,shape,XR_XIR_TARGET_UNSUPPORTED);fixture_release(old);
    const char *invalid[]={"", "\"\"\r\n", "C:/a.lib\r\n", "\"relative.lib\"\r\n", "\"C:a.lib\"\r\n",
        "\"\\root.lib\"\r\n", "\"C:/a.lib\"", "\"C:/a.lib\"\r", "\"C:/a.lib\"\n",
        " \"C:/a.lib\"\r\n", "\"C:/a.lib\" \r\n", "\r\n\"C:/a.lib\"\r\n",
        "\"C:/a.lib\"\r\n\r\n", "\"C:/a.lib\"\r\nx", "\"C:/a\"b.lib\"\r\n",
        "\"C:/a\t.lib\"\r\n", "\"\\\\server\\\"\r\n"};
    uint8_t bytes[512];
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        length=rsp_ascii(invalid[i],bytes);rsp_expect(bytes,length,shape,XR_XIR_TARGET_INVALID);
    }
    const char *unsupported[]={"\"/defaultlib:C:/a.lib\"\r\n","\"/LIBPATH:C:/libs\"\r\n",
        "\"-flag\"\r\n","\"@C:/a.rsp\"\r\n","\"C:/x@y.lib\"\r\n"};
    for(size_t i=0;i<sizeof(unsupported)/sizeof(*unsupported);++i) {
        length=rsp_ascii(unsupported[i],bytes);rsp_expect(bytes,length,shape,XR_XIR_TARGET_UNSUPPORTED);
    }
    length=rsp_ascii("\"C:/a.lib\"\r\n",bytes);
    for(size_t n=0;n<length;++n)rsp_expect(bytes,n,shape,XR_XIR_TARGET_INVALID);
    bytes[0]=0xfe;bytes[1]=0xff;rsp_expect(bytes,length,shape,XR_XIR_TARGET_UNSUPPORTED);
    bytes[0]=0;bytes[1]=0;rsp_expect(bytes,length,shape,XR_XIR_TARGET_INVALID);
    length=rsp_ascii("\"C:/a.lib\"\r\n",bytes);bytes[length]=0;
    rsp_expect(bytes,length+1,shape,XR_XIR_TARGET_INVALID);
    const uint16_t bad[]={0,1,0x7f,0x85,0xd800,0xdbff,0xdc00,0xdfff};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        length=rsp_ascii("\"C:/a.lib\"\r\n",bytes);bytes[10]=(uint8_t)bad[i];bytes[11]=(uint8_t)(bad[i]>>8);
        rsp_expect(bytes,length,shape,XR_XIR_TARGET_INVALID);
    }
    length=rsp_ascii("\"C:/a.lib\"\r\n",bytes);
    XrDependencyLimits exact={length,8,1},small=exact;
    rsp_expect(bytes,length,exact,XR_XIR_TARGET_OK);
    --small.frame_bytes;rsp_expect(bytes,length,small,XR_XIR_TARGET_BUDGET);
    small=exact;--small.path_bytes;rsp_expect(bytes,length,small,XR_XIR_TARGET_BUDGET);
    length=rsp_ascii("\"C:/a.lib\"\r\n\"C:/b.lib\"\r\n",bytes);
    small=shape;small.records=1;rsp_expect(bytes,length,small,XR_XIR_TARGET_BUDGET);
    XrDependencyInput input={XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,bytes,length,arguments,3};
    const char *response[]={"link.exe","@C:/args.rsp"};input.argv=response;input.argc=2;
    XrCompileResourceStats used;reset();CHECK(trial(&input,shape,&unlimited,&used)==XR_XIR_TARGET_UNSUPPORTED);
    input.argv=arguments;input.argc=3;
    XrCompileResourceLimits zero=unlimited;zero.work=0;bytes[0]=0;
    reset();CHECK(trial(&input,shape,&zero,&used)==XR_XIR_TARGET_BUDGET);
    reset();XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    XrDependencyFacts *sentinel=(XrDependencyFacts *)(uintptr_t)1;
    CHECK(xtc_dependencies_parse(r,&input,shape,&sentinel)==XR_XIR_TARGET_INVALID&&sentinel==(XrDependencyFacts *)(uintptr_t)1);
    xr_compile_resources_release(r);
}
static void rsp_work_and_lifetime(void) {
    uint8_t bytes[128];size_t length=rsp_ascii("\"C:/a\"\r\n",bytes);
    XrCompileResourceStats base=rsp_expect(bytes,length,shape,XR_XIR_TARGET_OK);
    length=rsp_ascii("\"C:/aa\"\r\n",bytes);
    XrCompileResourceStats longer=rsp_expect(bytes,length,shape,XR_XIR_TARGET_OK);
    /* One extra ASCII unit adds two copied/read UTF-16 bytes and one UTF-8
     * byte written, counted, validated and checked as a path character. */
    CHECK(longer.work-base.work==8);
    length=rsp_ascii("\"C:/a\"\r\n",bytes);bytes[10]=0xe9;
    longer=rsp_expect(bytes,length,shape,XR_XIR_TARGET_OK);CHECK(longer.work-base.work==4);
    bytes[10]=0x2d;bytes[11]=0x4e;
    longer=rsp_expect(bytes,length,shape,XR_XIR_TARGET_OK);CHECK(longer.work-base.work==8);
    length=rsp_ascii("\"C:/aa\"\r\n",bytes);bytes[10]=0x3d;bytes[11]=0xd8;bytes[12]=0;bytes[13]=0xde;
    longer=rsp_expect(bytes,length,shape,XR_XIR_TARGET_OK);CHECK(longer.work-base.work==16);
    XrDependencyLimits unicode_limit={length,7,1};rsp_expect(bytes,length,unicode_limit,XR_XIR_TARGET_OK);
    --unicode_limit.path_bytes;rsp_expect(bytes,length,unicode_limit,XR_XIR_TARGET_BUDGET);
    XrDependencyInput input={XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,bytes,length,arguments,3};
    boundaries(&input,true);
    char executable[]="C:/tools/link.exe",option[]="/NODEFAULTLIB";
    const char *argv[]={executable,option};input.argv=argv;input.argc=2;
    reset();XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    XrDependencyFacts *owner=NULL;CHECK(xtc_dependencies_parse(r,&input,shape,&owner)==XR_XIR_TARGET_OK);
    XrCompileResourceStats before,after;stats(r,&before);
    memset(executable,0xcd,sizeof(executable));memset(option,0xcd,sizeof(option));memset(bytes,0xcd,sizeof(bytes));
    xr_compile_resources_release(r);
    CHECK(!strcmp(xtc_dependencies_record(owner,0)->path,"C:/\xf0\x9f\x98\x80"));
    CHECK(xtc_dependencies_record(owner,0)->offset==4&&xtc_dependencies_count(owner)==1&&!xtc_dependencies_target(owner));
    stats(xtc_dependencies_resources(owner),&after);CHECK(before.work==after.work);
    xtc_dependencies_free(owner);reset();
    length=rsp_ascii("\"C:/a\"\r\n",bytes);
    input=(XrDependencyInput){XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,bytes,length,arguments,3};
    XrCompileResourceLimits once=unlimited;once.allocated_bytes=base.allocated_bytes;
    r=NULL;CHECK(xr_compile_resources_new(&once,&r)==XR_COMPILE_RESOURCE_OK);
    owner=NULL;CHECK(xtc_dependencies_parse(r,&input,shape,&owner)==XR_XIR_TARGET_OK);
    xtc_dependencies_free(owner);owner=NULL;
    CHECK(xtc_dependencies_parse(r,&input,shape,&owner)==XR_XIR_TARGET_BUDGET&&!owner);
    xr_compile_resources_release(r);reset();
    printf("RSP independent UTF-16/UTF-8 work deltas, producer lifetime and malformed frames PASS\n");
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    cursor_fixed_work();
    captured(argv[1],"msvc",XR_DEPENDENCY_MSVC_SOURCE_1_2,229);
    captured(argv[1],"clang",XR_DEPENDENCY_WINDOWS_MAKE,182);
    captured(argv[1],"zig",XR_DEPENDENCY_WINDOWS_MAKE,182);
    captured(argv[1],"msvc-link",XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,35);
    captured(argv[1],"msvc-fullpath",XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,12);
    captured(argv[1],"clang-fullpath",XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,12);
    captured(argv[1],"zig-fullpath",XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,12);
    captured(argv[1],"unicode-fullpath",XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE,6);
    rsp_rejections(argv[1]);rsp_work_and_lifetime();
    rejection_and_work(); reset();
    printf("dependency facts passed; actual OOM=%zu; work cutoffs=%zu\n",all_oom,all_work);
    return 0;
}
