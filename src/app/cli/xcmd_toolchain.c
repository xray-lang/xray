/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain.c - Public consumers of owned toolchain preferences
 */
#include "xcli_spec.h"
#include "xcli_toolchain_config.h"
#include "xcli_toolchain_update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool toolchain_json(const XrCliInvocation *inv) {
    return (inv->ctx && inv->ctx->json_output) || xr_cli_opt_bool(&inv->options,"json");
}
static void toolchain_string(const char *value) {
    if (!value) { fputs("null",stdout); return; }
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)value;*p;++p) {
        if (*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if (*p<32) printf("\\u%04x",(unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}
static bool toolchain_options(const XrCliInvocation *inv,bool target,bool paths) {
    if (inv->passthrough_argc) return false;
    for (int i=0;i<inv->options.count;++i) {
        if (!inv->options.present[i]) continue;
        const char *name=inv->options.spec[i].long_name;
        if (!strcmp(name,"json") || (target && !strcmp(name,"target")) ||
            (paths && (!strcmp(name,"cc") || !strcmp(name,"zig")))) continue;
        return false;
    }
    return true;
}
static int toolchain_read(XrCompileResources *resources,const XrCliInvocation *inv,bool path_only) {
    if (inv->positional_count!=1 || !toolchain_options(inv,false,false)) return XR_CLI_EXIT_USAGE;
    char *path=NULL;XrCliToolchainConfig *owner=NULL;
    XrCliToolchainConfigStatus status=path_only ? xr_cli_toolchain_config_path(resources,NULL,&path) :
        xr_cli_toolchain_config_open(resources,NULL,&owner);
    if (status!=XR_CLI_TOOLCHAIN_CONFIG_OK) {
        xr_cli_error("toolchain","preference lookup failed (status=%u)",(unsigned)status);
        return status==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
    }
    const XrCliToolchainConfigView *view=path_only ? NULL : xr_cli_toolchain_config_view(owner);
    if (toolchain_json(inv)) {
        printf("{\"schema\":1,\"%s\":",path_only ? "path" : "configPath");
        toolchain_string(path_only ? path : view->path);
        if (view) {
            printf(",\"exists\":%s,\"preferences\":[",view->exists ? "true" : "false");
            for (size_t i=0;i<view->count;++i) {
                const XrCliToolchainPreference *preference=&view->preferences[i];
                if (i) putchar(',');
                fputs("{\"target\":",stdout);toolchain_string(preference->target);
                fputs(",\"provider\":",stdout);toolchain_string(xtc_selector_name(preference->selector));
                fputs(",\"compiler\":",stdout);toolchain_string(preference->compiler);
                fputs(",\"zig\":",stdout);toolchain_string(preference->zig);putchar('}');
            }
            printf("],\"allowFallback\":%s,\"preferExternalNative\":%s,\"autoProbe\":%s",
                view->allow_fallback ? "true" : "false",view->prefer_external_native ? "true" : "false",
                view->auto_probe ? "true" : "false");
        }
        puts("}");
    } else if (path_only) puts(path);
    else {
        printf("Toolchain preferences: %s\n",view->path);
        for (size_t i=0;i<view->count;++i)
            printf("%s -> %s\n",view->preferences[i].target,xtc_selector_name(view->preferences[i].selector));
    }
    xr_compile_resources_free(path);xr_cli_toolchain_config_free(owner);return XR_CLI_EXIT_OK;
}
#if XR_OS_WINDOWS
static int toolchain_update_close(XrCliToolchainUpdate **owner) {
    for (unsigned attempt=0;attempt<8;++attempt)
        if (xr_cli_toolchain_update_close(owner)==XR_CLI_TOOLCHAIN_UPDATE_OK) return XR_CLI_EXIT_OK;
    fputs("toolchain: preference cleanup remains pending\n",stderr);fflush(stderr);
    /* A command cannot discard an outstanding lock or publication owner. */
    _Exit(XR_CLI_EXIT_INTERNAL);
}
static int toolchain_write(XrCompileResources *resources,const XrCliInvocation *inv,bool reset) {
    const char *target=xr_cli_opt_string(&inv->options,"target",NULL);
    if (inv->positional_count!=(reset ? 1 : 2) || !toolchain_options(inv,true,!reset) ||
        (!reset && !target)) return XR_CLI_EXIT_USAGE;
    XrToolchainSelector selector=XR_TOOLCHAIN_SELECTOR_AUTO;char error[256]={0};
    const char *compiler=xr_cli_opt_string(&inv->options,"cc",NULL);
    const char *zig=xr_cli_opt_string(&inv->options,"zig",NULL);
    if (!reset && (!xtc_selector_parse(inv->positionals[1],&selector,error,sizeof(error)) ||
        (compiler && selector==XR_TOOLCHAIN_SELECTOR_ZIG) ||
        (zig && selector!=XR_TOOLCHAIN_SELECTOR_ZIG && selector!=XR_TOOLCHAIN_SELECTOR_AUTO))) {
        xr_cli_error("toolchain","invalid provider or provider/path combination");return XR_CLI_EXIT_USAGE;
    }
    XrCliToolchainUpdateRequest request={NULL,target,compiler,zig,selector,reset};
    XrCliToolchainUpdate *owner=NULL;
    XrCliToolchainUpdateStatus status=xr_cli_toolchain_update_new(resources,&request,&owner);
    if (status==XR_CLI_TOOLCHAIN_UPDATE_OK) status=xr_cli_toolchain_update_execute(owner);
    int result=XR_CLI_EXIT_OK;
    if (status!=XR_CLI_TOOLCHAIN_UPDATE_OK) {
        xr_cli_error("toolchain","preference update failed (status=%u)",(unsigned)status);
        result=status==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
    } else if (toolchain_json(inv)) {
        printf("{\"schema\":1,\"%s\":true,\"target\":",reset ? "reset" : "updated");toolchain_string(target);
        if (!reset) { fputs(",\"provider\":",stdout);toolchain_string(xtc_selector_name(selector)); }
        fputs(",\"configPath\":",stdout);toolchain_string(xr_cli_toolchain_update_view(owner)->path);puts("}");
    } else printf("Toolchain preference %s: %s\n",reset ? "reset" : "updated",target ? target : "all");
    int closed=toolchain_update_close(&owner);return closed>result ? closed : result;
}
#endif
XR_FUNC int cmd_toolchain(const XrCliInvocation *inv) {
    if (!inv || !inv->positionals || inv->positional_count<1 ||
        (inv->options.count && (!inv->options.spec || !inv->options.present))) return XR_CLI_EXIT_USAGE;
    XrCompileResourceLimits limits={67108864,16777216,128000000};XrCompileResources *resources=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if (created!=XR_COMPILE_RESOURCE_OK) return XR_CLI_EXIT_INTERNAL;
    const char *command=inv->positionals[0];int result=XR_CLI_EXIT_USAGE;
    if (!strcmp(command,"config-path") || !strcmp(command,"list"))
        result=toolchain_read(resources,inv,!strcmp(command,"config-path"));
#if XR_OS_WINDOWS
    else if (!strcmp(command,"use") || !strcmp(command,"reset"))
        result=toolchain_write(resources,inv,!strcmp(command,"reset"));
#endif
    else xr_cli_error("toolchain","unsupported subcommand '%s'",command);
    xr_compile_resources_release(resources);
    if (fflush(stdout) || ferror(stdout) || ferror(stderr)) return XR_CLI_EXIT_FAIL;
    return result;
}
