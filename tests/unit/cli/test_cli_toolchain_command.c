/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_toolchain_command.c - Physical faults at the real command boundary
 */
#include "app/cli/xcli_spec.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void *physical_malloc(size_t size){return xr_malloc(size);}
static void physical_free(void *memory){xr_free(memory);}
typedef struct CommandBlock {void *pointer;size_t size;} CommandBlock;
static CommandBlock blocks[4096];
static size_t attempts,fail_at=SIZE_MAX,live,physical_bytes;
static void *command_malloc(size_t size) {
    if(attempts++==fail_at)return NULL;
    void *memory=physical_malloc(size);if(!memory)return NULL;
    for(size_t i=0;i<4096;++i)if(!blocks[i].pointer){blocks[i]=(CommandBlock){memory,size};++live;physical_bytes+=size;return memory;}
    CHECK(false);return NULL;
}
static void command_free(void *memory) {
    if(!memory)return;
    for(size_t i=0;i<4096;++i)if(blocks[i].pointer==memory){--live;physical_bytes-=blocks[i].size;blocks[i]=(CommandBlock){0};physical_free(memory);return;}
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc command_malloc
#define xr_free command_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
XR_FUNC int cmd_toolchain(const XrCliInvocation *inv);
static const char initial[]="schema=1\n[native]\nprovider=\"msvc\"\n[target.\"aarch64-windows-msvc\"]\nprovider=\"msvc\"\n";
static void restore(const char *path) {
    FILE *file=fopen(path,"wb");CHECK(file);CHECK(fwrite(initial,1,sizeof(initial)-1,file)==sizeof(initial)-1);CHECK(!fclose(file));
}
static bool unchanged(const char *path) {
    FILE *file=fopen(path,"rb");if(!file)return false;
    char buffer[sizeof(initial)+1];size_t size=fread(buffer,1,sizeof(buffer),file);CHECK(!ferror(file));CHECK(!fclose(file));
    return size==sizeof(initial)-1 && !memcmp(buffer,initial,size);
}
/* Options are addressed by name so probe-only options never shift the preference slots. */
static size_t option_slot(const XrCliOptionSpec *options,const char *name) {
    for(size_t i=0;options[i].long_name;++i)if(!strcmp(options[i].long_name,name))return i;
    CHECK(false);return 0;
}
static void run(const char *path,unsigned family,bool resources) {
    const XrCliCommandSpec *spec=xr_cli_find_command("toolchain");CHECK(spec && xr_cli_option_count(spec->options)==7);
    const char *positionals[]={family==0 ? "use" : family<3 ? "reset" : family==3 ? "list" : "config-path","clang"};
    bool present[7]={false};const char *values[7]={NULL};
    size_t target=option_slot(spec->options,"target"),cc=option_slot(spec->options,"cc");
    present[target]=family<2;values[target]="native";
    present[cc]=family==0;values[cc]="C:\\compiler\\quoted\"中\".exe";
    present[option_slot(spec->options,"json")]=true;
    XrCliContext context={0};XrCliInvocation inv={.ctx=&context,.spec=spec,.options={spec->options,7,present,values},
        .positional_count=family==0 ? 2 : 1,.positionals=positionals};
    size_t sites=0,observed=0;
    for(size_t point=0;point<=sites;++point) {
        restore(path);CHECK(!live && !physical_bytes);attempts=0;fail_at=point ? point-1 : SIZE_MAX;
        int result=cmd_toolchain(&inv);
        if(!point){CHECK(result==XR_CLI_EXIT_OK);observed=attempts;sites=resources ? observed : 0;}
        else {CHECK(result==XR_CLI_EXIT_FAIL || result==XR_CLI_EXIT_UNAVAILABLE);CHECK(attempts>fail_at);CHECK(unchanged(path));}
        CHECK(!live && !physical_bytes);
    }
    fail_at=SIZE_MAX;
    fprintf(stderr,"Public command family%u observed=%zu swept=%zu physical=0/0\n",family,observed,sites);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"--resources")));
    CHECK(freopen("NUL","wb",stdout));
    for(unsigned family=0;family<5;++family)run(argv[1],family,argc==3);
    return 0;
}
