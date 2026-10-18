/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_publication_faults.c - Real service publication and heap ownership
 *
 * KEY CONCEPT:
 *   Descriptor-backed transcripts exercise the production server and encoder.
 *   The allocator observes every MCP allocation and last-owner release.
 */
#include "app/mcp/xmcp_json.h"
#include "app/mcp/xmcp_server.h"
#include "app/mcp/xmcp_protocol.h"
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#define descriptor _fileno
#else
#include <unistd.h>
#define descriptor fileno
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation owned[32768];
static size_t attempts, fail_at=SIZE_MAX, live, physical_bytes;
static bool injected;
static size_t find_pointer(void *pointer) {
    for(size_t i=0;i<32768;++i) if(owned[i].pointer==pointer) return i;
    CHECK(false);return 0;
}
static void record(void *pointer,size_t bytes) {
    if(!pointer)return;
    for(size_t i=0;i<32768;++i) if(!owned[i].pointer) {
        owned[i]=(Allocation){pointer,bytes};++live;physical_bytes+=bytes;return;
    }
    CHECK(false);
}
static bool fail(void) { if(attempts++==fail_at){injected=true;return true;}return false; }
XR_FUNC void *mcp_fault_malloc(size_t size) { if(fail())return NULL;void *p=xr_malloc(size);record(p,size);return p; }
XR_FUNC void *mcp_fault_calloc(size_t count,size_t size) {
    if(fail())return NULL;CHECK(!size || count<=SIZE_MAX/size);
    void *p=xr_calloc(count,size);record(p,count*size);return p;
}
XR_FUNC void *mcp_fault_realloc(void *pointer,size_t bytes) {
    if(fail())return NULL;
    size_t index=pointer?find_pointer(pointer):SIZE_MAX;
    void *p=xr_realloc(pointer,bytes);
    if(p){if(pointer){physical_bytes-=owned[index].bytes;owned[index]=(Allocation){p,bytes};physical_bytes+=bytes;}
        else record(p,bytes);}
    return p;
}
XR_FUNC void mcp_fault_free(void *pointer) {
    if(pointer){size_t index=find_pointer(pointer);physical_bytes-=owned[index].bytes;owned[index]=(Allocation){0};--live;}
    xr_free(pointer);
}
XR_FUNC char *mcp_fault_strdup(const char *text) {
    size_t size=strlen(text)+1;char *p=mcp_fault_malloc(size);if(p)memcpy(p,text,size);return p;
}

static void reset_fault(size_t ordinal) {
    attempts=0; injected=false; fail_at=ordinal; xmcp_json_begin();
}
static void released(void) { CHECK(!live && !physical_bytes); xmcp_json_begin(); }
static XmcpServer *server_new(void) {
    XmcpServerOptions options; xmcp_server_options_default(&options);
    return xmcp_server_new(&options);
}
static void factory_one(void) {
    reset_fault(2);
    XmcpServer *server=server_new();
    CHECK(injected && !server);
    fail_at=SIZE_MAX; xmcp_server_free(server); released();
}
static void factory_faults(void) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        reset_fault(probe?probe-1:SIZE_MAX);
        XmcpServer *server=server_new();
        if(!probe) { CHECK(server && server->knowledge); sites=attempts; }
        else CHECK(injected && !server);
        fail_at=SIZE_MAX; xmcp_server_free(server); released();
    }
    printf("MCP factory: %zu complete faults; no incomplete service; physical0\n",sites);
}
static void clone_faults(void) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        reset_fault(SIZE_MAX);
        XrJsonValue *value=xjson_new_array(); CHECK(value);
        CHECK(xmcp_json_push(value,xjson_new_string_n("a\0b",3)));
        XrJsonValue *number=xjson_new_number(0); CHECK(number);
        number->is_integer=true; number->as.integer=INT64_C(9007199254740993);
        CHECK(xmcp_json_push(value,number));
        reset_fault(probe?probe-1:SIZE_MAX);
        XrJsonValue *copy=xmcp_json_clone(value);
        if(!probe) {
            CHECK(copy && copy->as.array.count==2); sites=attempts;
            XrJsonValue *text=xjson_array_get(copy,0), *integer=xjson_array_get(copy,1);
            CHECK(text->string_len==3 && !memcmp(text->as.string,"a\0b",3));
            CHECK(integer->is_integer && integer->as.integer==INT64_C(9007199254740993));
        } else CHECK(injected && !copy && xmcp_json_failed());
        fail_at=SIZE_MAX; xjson_free(copy); xjson_free(value); released();
    }
    printf("MCP owned clone: %zu complete faults; exact bytes/integer; physical0\n",sites);
}
static void notification_faults(void) {
    size_t sites=0; char message[8193]; memset(message,'x',sizeof(message)-1);message[sizeof(message)-1]=0;
    for(size_t probe=0;probe<=sites;++probe) {
        reset_fault(SIZE_MAX);
        XmcpServer *server=server_new(); CHECK(server);
        FILE *output=tmpfile(); CHECK(output);
        server->transport.write_fd=descriptor(output); server->lifecycle_state=XMCP_LIFECYCLE_READY;
        reset_fault(probe?probe-1:SIZE_MAX);
        xmcp_send_log_notification(server,"info",message);
        fail_at=SIZE_MAX;
        CHECK(!fseek(output,0,SEEK_END)); long size=ftell(output); CHECK(size>=0);
        if(!probe) {
            CHECK(size>8192 && !server->failed); sites=attempts;
            CHECK(!fseek(output,0,SEEK_SET)); char bytes[16384];
            CHECK((size_t)size<sizeof(bytes) && fread(bytes,1,(size_t)size,output)==(size_t)size);
            XrJsonValue *wire=xjson_parse(bytes,(size_t)size); CHECK(wire);
            CHECK(!strcmp(xjson_get_string(wire,"method"),"notifications/message"));
            CHECK(!strcmp(xjson_get_string(xjson_get(wire,"params"),"data"),message)); xjson_free(wire);
        } else CHECK(injected && size==0 && server->failed && server->shutdown);
        fclose(output); xmcp_server_free(server); released();
    }
    printf("MCP notification: %zu complete faults; atomic encoded publication; physical0\n",sites);
}
static void response_faults(void) {
    const char *request="{\"jsonrpc\":\"2.0\",\"id\":\"a\\u0000b\",\"method\":\"ping\"}\n";
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        reset_fault(SIZE_MAX); XmcpServer *server=server_new(); CHECK(server);
        FILE *input=tmpfile(),*output=tmpfile(); CHECK(input && output);
        CHECK(fputs(request,input)>=0 && !fflush(input) && !fseek(input,0,SEEK_SET));
        server->transport.read_fd=descriptor(input);server->transport.write_fd=descriptor(output);server->log_level=-1;
        reset_fault(probe?probe-1:SIZE_MAX);
        int status=xmcp_server_run(server); size_t production_sites=attempts; fail_at=SIZE_MAX;
        CHECK(!fseek(output,0,SEEK_END));long size=ftell(output); CHECK(size>=0);
        XrJsonValue *wire=NULL;
        if(size) {
            CHECK(!fseek(output,0,SEEK_SET));char bytes[4096]; CHECK((size_t)size<sizeof(bytes));
            CHECK(fread(bytes,1,(size_t)size,output)==(size_t)size);
            wire=xjson_parse(bytes,(size_t)size);CHECK(wire && !strcmp(xjson_get_string(wire,"jsonrpc"),"2.0"));
        }
        if(!probe) {
            CHECK(status==0 && wire && xjson_get(wire,"result"));sites=production_sites;
            XrJsonValue *id=xjson_get(wire,"id");CHECK(id && id->string_len==3 && !memcmp(id->as.string,"a\0b",3));
        } else {
            CHECK(injected && (!wire || xjson_get(wire,"error")));
            if(!wire)CHECK(status==1 && server->failed);
            /* The shared parser currently reports both syntax and allocation failure as NULL. */
            if(wire)CHECK(xjson_get_int_or(xjson_get(wire,"error"),"code",0)==XMCP_ERR_PARSE);
        }
        xjson_free(wire);fclose(input);fclose(output);xmcp_server_free(server);released();
    }
    printf("MCP ping response: %zu complete faults; no half success; physical0\n",sites);
}
static int parser_array_one(void) {
    reset_fault(2);
    XrJsonValue *array=xjson_parse("[42]",4); CHECK(injected);
    fail_at=SIZE_MAX;
    int count=array?array->as.array.count:-1;
    xjson_free(array);
    bool failed=array!=NULL || live!=0;
    printf("Shared parser array growth fault: published count=%d; live after ordinary release=%zu; qualification=%s\n",
           count,live,failed?"FAIL":"PASS");
    /* This probe cleanup does not count as a production last-owner release. */
    for(size_t i=0;i<32768;++i) if(owned[i].pointer)mcp_fault_free(owned[i].pointer);
    released();return failed?1:0;
}
static void base_builder_faults(void) {
    for (int mode=0; mode<3; ++mode) {
        size_t sites=0;
        for (size_t probe=0; probe<=sites; ++probe) {
            reset_fault(SIZE_MAX);
            XrJsonValue *parent=mode?xjson_new_object():xjson_new_array(); CHECK(parent);
            for (int i=0; i<8; ++i) {
                XrJsonValue *child=xjson_new_number(42+i); CHECK(child);
                char key[16]; CHECK(snprintf(key,sizeof(key),"keep%d",i)>0);
                CHECK(mode?xjson_object_set_new(parent,key,child):xjson_array_push(parent,child));
            }
            XrJsonValue *kept=mode?xjson_get(parent,"keep0"):xjson_array_get(parent,0);
            if (mode) { CHECK(xjson_object_set(parent,"keep0",kept)); CHECK(kept->as.number==42); }
            void *storage=mode?(void *)parent->as.object.members:(void *)parent->as.array.items;
            XrJsonValue *child=xjson_new_number(99); CHECK(child);
            reset_fault(probe?probe-1:SIZE_MAX);
            bool accepted=mode==0?xjson_array_push(parent,child):
                mode==1?xjson_object_set(parent,"added",child):xjson_object_set_new(parent,"added",child);
            if (!probe) {
                CHECK(accepted); sites=attempts;
                CHECK(mode?xjson_get(parent,"added")==child:xjson_array_get(parent,8)==child);
            } else {
                CHECK(injected && !accepted && child->as.number==99);
                CHECK(mode?parent->as.object.count==8:parent->as.array.count==8);
                CHECK(mode?parent->as.object.capacity==8:parent->as.array.capacity==8);
                CHECK(storage==(mode?(void *)parent->as.object.members:(void *)parent->as.array.items));
                CHECK(mode?xjson_get(parent,"keep0")==kept:xjson_array_get(parent,0)==kept);
                CHECK(kept->as.number==42);
                fail_at=SIZE_MAX; xjson_free(child);
            }
            fail_at=SIZE_MAX; xjson_free(parent); released();
        }
        printf("Base JSON insertion%d: %zu complete faults; failure preserves both owners; physical0\n",mode,sites);
    }
}
static void check_parsed_case(XrJsonValue *value,int index) {
    CHECK(value);
    if (index<2) {
        CHECK(xjson_array_len(value)==(index?9:1));
        XrJsonValue *last=xjson_array_get(value,index?8:0);
        CHECK(last && last->is_integer && last->as.integer==(index?9:42));
    } else {
        CHECK(xjson_get_int(value,"tail")==7);
        XrJsonValue *items=xjson_get(value,"items"); CHECK(xjson_array_len(items)==3);
        XrJsonValue *text=xjson_get(xjson_array_get(items,1),"text");
        CHECK(text && text->string_len==3 && !memcmp(text->as.string,"a\0b",3));
        CHECK(xjson_array_len(xjson_array_get(items,2))==3);
    }
}
static void base_parser_faults(void) {
    const char *cases[]={"[42]","[1,2,3,4,5,6,7,8,9]",
        "{\"items\":[42,{\"text\":\"a\\u0000b\"},[true,null,-9]],\"tail\":7}"};
    for (int index=0; index<3; ++index) {
        size_t sites=0;
        for (size_t probe=0; probe<=sites; ++probe) {
            reset_fault(probe?probe-1:SIZE_MAX);
            XrJsonValue *value=xjson_parse(cases[index],strlen(cases[index]));
            if (!probe) { check_parsed_case(value,index); sites=attempts; }
            else CHECK(injected && !value);
            fail_at=SIZE_MAX; xjson_free(value); released();
            reset_fault(SIZE_MAX);
            value=xjson_parse(cases[index],strlen(cases[index])); check_parsed_case(value,index);
            xjson_free(value); released();
        }
        printf("Base JSON parser%d: %zu complete faults; no partial publication; physical0 and recovery\n",index,sites);
    }
}
static void base_clone_faults(void) {
    const char *text="{\"items\":[42,{\"text\":\"a\\u0000b\"},[true,null,-9]],\"tail\":7}";
    size_t sites=0;
    for (size_t probe=0; probe<=sites; ++probe) {
        reset_fault(SIZE_MAX); XrJsonValue *value=xjson_parse(text,strlen(text)); check_parsed_case(value,2);
        reset_fault(probe?probe-1:SIZE_MAX); XrJsonValue *copy=xjson_clone(value);
        if (!probe) { check_parsed_case(copy,2); sites=attempts; }
        else CHECK(injected && !copy);
        fail_at=SIZE_MAX; check_parsed_case(value,2); xjson_free(copy); xjson_free(value); released();
    }
    printf("Base JSON clone: %zu complete faults; source preserved; physical0\n",sites);
}
static void base_json_faults(void) {
    CHECK(parser_array_one()==0);
    base_builder_faults(); base_parser_faults(); base_clone_faults();
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if(argc==2 && !strcmp(argv[1],"--factory-one")){factory_one();return 0;}
    if(argc==2 && !strcmp(argv[1],"--parse-array-one"))return parser_array_one();
    if(argc==2 && !strcmp(argv[1],"--base-json")){base_json_faults();return 0;}
    factory_faults();clone_faults();notification_faults();response_faults();base_json_faults();return 0;
}
