/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdio_owner.c - Real stdio framing failure prefixes and mode ownership
 */
#define main reference_json_main
#include "test_json.c"
#undef main
#include "app/dap/xdap_stdio.h"
#ifdef XR_OS_WINDOWS
#include <io.h>
#include <fcntl.h>
#define stdio_dup _dup
#define stdio_dup2 _dup2
#define stdio_close _close
#define stdio_fileno _fileno
#else
#include <unistd.h>
#define stdio_dup dup
#define stdio_dup2 dup2
#define stdio_close close
#define stdio_fileno fileno
#endif
typedef struct StdioInput { FILE *file;int saved; } StdioInput;
static StdioInput input_begin(const char *bytes,size_t length) {
    StdioInput input={tmpfile(),stdio_dup(stdio_fileno(stdin))};CHECK(input.file && input.saved>=0);
    CHECK(fwrite(bytes,1,length,input.file)==length && !fflush(input.file));rewind(input.file);
    CHECK(stdio_dup2(stdio_fileno(input.file),stdio_fileno(stdin))>=0);clearerr(stdin);return input;
}
static void input_end(StdioInput *input) {
    CHECK(stdio_dup2(input->saved,stdio_fileno(stdin))>=0);CHECK(!stdio_close(input->saved));
    CHECK(!fclose(input->file));clearerr(stdin);
}
static size_t stdio_operation(unsigned mode,size_t fail) {
    char bytes[7000];size_t body=mode==2 ? 6000 : 2;
    int header=snprintf(bytes,sizeof(bytes),"Content-Length: %zu\r\n\r\n",body);CHECK(header>0);
    memset(bytes+header,'a',body);StdioInput input=input_begin(bytes,(size_t)header+body);
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapStdio *io=NULL;
    if (mode) CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=XR_XIR_OK;char *message=NULL;size_t length=777;bool eof=false;
    if (!mode) status=xdap_stdio_new(owner,8192,&io);
    else for (unsigned i=0;i<10 && !message && status==XR_XIR_OK;++i) status=xdap_stdio_read(io,&message,&length,&eof);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK);if (mode) CHECK(message && length==body); }
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !message && length==777);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(owner,&spent)==XR_COMPILE_RESOURCE_OK);
        if (!mode) { CHECK(!io && spent.held_bytes==baseline.held_bytes);CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_OK); }
        else {
            for (unsigned i=0;i<10 && !message;++i) CHECK(xdap_stdio_read(io,&message,&length,&eof)==XR_XIR_OK);
            CHECK(message && length==body);
        }
        XrCompileResourceStats retry={0};CHECK(xr_compile_resources_stats(owner,&retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(retry.allocated_bytes>=spent.allocated_bytes);
    }
    if (message) for (size_t i=0;i<length;++i) CHECK(message[i]=='a');
    xr_compile_resources_free(message);xdap_stdio_free(io);input_end(&input);owner_free(owner,baseline);return sites;
}
static void rejected_prefix(void) {
    static const char *bad[]={"Content-Length: 2junk\r\n\r\naa", "Content-Length: 2\r\ncontent-length: 2\r\n\r\naa",
        "X-Content-Length: 2\r\n\r\naa","Content-Length: 20000\r\n\r\n"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        StdioInput input=input_begin(bad[i],strlen(bad[i]));XrCompileResourceStats baseline;
        XrCompileResources *owner=owner_new(&baseline);XdapStdio *io=NULL;CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_OK);
        for (unsigned retry=0;retry<2;++retry) {
            char *message=NULL;size_t length=777;bool eof=false;
            CHECK(xdap_stdio_read(io,&message,&length,&eof)==(i==3 ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE));
            CHECK(!message && length==777);
        }
        xdap_stdio_free(io);input_end(&input);owner_free(owner,baseline);
    }
}
static void mode_failures(void) {
    for (unsigned output=0;output<2;++output) {
        int fd=stdio_fileno(output ? stdout : stdin),saved=stdio_dup(fd);CHECK(saved>=0 && !stdio_close(fd));
        XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapStdio *io=NULL;
        XrXirStatus status=xdap_stdio_new(owner,8192,&io);
#ifdef XR_OS_WINDOWS
        CHECK(status==XR_XIR_IO && !io);
#else
        CHECK(output ? status==XR_XIR_OK : status==XR_XIR_IO);
#endif
        xdap_stdio_free(io);CHECK(stdio_dup2(saved,fd)>=0 && !stdio_close(saved));
        clearerr(output ? stdout : stdin);owner_free(owner,baseline);
    }
}
static void finite_owner(void) {
    StdioInput input=input_begin("",0);
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);
    XdapStdio *io=NULL;CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_OK);
    XrCompileResourceStats opened=sdk_stats(owner);xdap_stdio_free(io);owner_free(owner,baseline);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,baseline.held_bytes,true};
    owner=sdk_ledger(&limits);io=NULL;size_t attempts=runtime_attempts;
    CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_MEMORY_LIMIT && !io);
    CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes && runtime_attempts==attempts);
    owner_free(owner,baseline);
    limits.memory_limit=opened.held_bytes;owner=sdk_ledger(&limits);
    CHECK(xdap_stdio_new(owner,8192,&io)==XR_XIR_OK && io);
    xr_compile_resources_release(owner);xdap_stdio_free(io);
    CHECK(!runtime_live && !runtime_bytes);
    input_end(&input);
    puts("Finite stdio constructor: owner-only held rejection before malloc, exact resident admission and producer-dead cleanup PASS");
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--resources")) {
        for (unsigned mode=0;mode<3;++mode) {
            size_t sites=stdio_operation(mode,SIZE_MAX);CHECK(sites);
            for (size_t i=0;i<sites;++i) stdio_operation(mode,i);
            printf("Stdio allocation slice %u: %zu sites; owned retry and physical baseline PASS\n",mode,sites);
        }
    } else { CHECK(argc==1);rejected_prefix();mode_failures();finite_owner();puts("Strict stdio repeated rejection and mode failure ownership: PASS"); }
    return 0;
}
