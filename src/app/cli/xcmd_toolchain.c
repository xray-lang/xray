/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain.c - Owned toolchain preferences at the public CLI boundary
 */
#include "xcli_spec.h"
#include "xcli_diag.h"
#include "xcli_toolchain_config.h"
#include "xcli_toolchain_update.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool toolchain_json(const XrCliInvocation *inv) {
    return (inv->ctx && inv->ctx->json_output) || xr_cli_opt_bool(&inv->options,"json");
}
static void toolchain_string(const char *text) {
    if(!text){fputs("null",stdout);return;}
    putchar('"');
    for(const unsigned char *p=(const unsigned char *)text;*p;++p) {
        if(*p=='"' || *p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",(unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}
static int toolchain_output_status(void) {
    if(fflush(stdout)==0 && !ferror(stdout))return XR_CLI_EXIT_OK;
    xr_cli_error("toolchain","preference output failed");return XR_CLI_EXIT_FAIL;
}
static int toolchain_config_error(XrCliToolchainConfigStatus status) {
    const char *reason=status==XR_CLI_TOOLCHAIN_CONFIG_INVALID ? "invalid preference configuration" :
        status==XR_CLI_TOOLCHAIN_CONFIG_BUDGET ? "preference resource budget exhausted" :
        status==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY ? "preference allocation failed" :
        status==XR_CLI_TOOLCHAIN_CONFIG_UNRESOLVED ? "preference path is unresolved" : "preference I/O failed";
    xr_cli_error("toolchain","%s",reason);return XR_CLI_EXIT_FAIL;
}
static bool toolchain_target(const char *text) {
    XrToolchainTarget target;char error[256];
    if(!text)return true;
    if(!*text){xr_cli_error("toolchain","preference target is empty");return false;}
    if(xtc_target_parse(text,&target,error,sizeof(error)))return true;
    xr_cli_error("toolchain","%s",error);return false;
}
static int toolchain_read(const XrCliInvocation *inv,XrCompileState *state,bool path_only) {
    const char *target=xr_cli_opt_string(&inv->options,"target",NULL);
    if(path_only) {
        char *path=NULL;XrCliToolchainConfigStatus status=xr_cli_toolchain_config_path(state,NULL,&path);
        if(status!=XR_CLI_TOOLCHAIN_CONFIG_OK)return toolchain_config_error(status);
        if(toolchain_json(inv)){fputs("{\"schema\":1,\"path\":",stdout);toolchain_string(path);puts("}");}
        else puts(path);
        xr_compile_resources_free(path);return toolchain_output_status();
    }
    if(!toolchain_target(target))return XR_CLI_EXIT_USAGE;
    XrCliToolchainConfig *owner=NULL;
    XrCliToolchainConfigStatus status=xr_cli_toolchain_config_open(state,NULL,&owner);
    if(status!=XR_CLI_TOOLCHAIN_CONFIG_OK)return toolchain_config_error(status);
    const XrCliToolchainConfigView *view=xr_cli_toolchain_config_view(owner);bool json=toolchain_json(inv),first=true;
    if(json) {
        fputs("{\"schema\":1,\"configPath\":",stdout);toolchain_string(view->path);
        printf(",\"exists\":%s,\"policy\":{\"allowFallback\":%s,\"preferExternalNative\":%s,\"autoProbe\":%s},\"preferences\":[",
            view->exists ? "true" : "false",view->allow_fallback ? "true" : "false",
            view->prefer_external_native ? "true" : "false",view->auto_probe ? "true" : "false");
    } else printf("Toolchain preferences: %s\n",view->path);
    for(size_t i=0;i<view->count;++i) {
        const XrCliToolchainPreference *row=&view->preferences[i];
        if(target && strcmp(target,row->target))continue;
        if(json) {
            if(!first)putchar(',');
            fputs("{\"target\":",stdout);toolchain_string(row->target);
            fputs(",\"provider\":",stdout);toolchain_string(xtc_selector_name(row->selector));
            fputs(",\"compiler\":",stdout);toolchain_string(row->compiler);
            fputs(",\"zig\":",stdout);toolchain_string(row->zig);putchar('}');
        } else printf("  %s -> %s%s%s%s%s\n",row->target,xtc_selector_name(row->selector),
            row->compiler ? " compiler=" : "",row->compiler ? row->compiler : "",
            row->zig ? " zig=" : "",row->zig ? row->zig : "");
        first=false;
    }
    if(json)puts("]}");
    xr_cli_toolchain_config_free(owner);return toolchain_output_status();
}
#ifdef XR_OS_WINDOWS
#include "xcmd_toolchain_update.inc.c"
#endif
XR_FUNC int cmd_toolchain(const XrCliInvocation *inv) {
    if(!inv || !inv->positional_count)return XR_CLI_EXIT_USAGE;
    const char *action=inv->positionals[0];bool use=!strcmp(action,"use"),reset=!strcmp(action,"reset");
    bool list=!strcmp(action,"list"),path_only=!strcmp(action,"config-path");
    if((!use && !reset && !list && !path_only) || inv->positional_count!=(use ? 2 : 1) ||
        (!use && (xr_cli_opt_present(&inv->options,"cc") || xr_cli_opt_present(&inv->options,"zig"))) ||
        (path_only && xr_cli_opt_present(&inv->options,"target")) ||
        (use && !xr_cli_opt_present(&inv->options,"target"))) {
        xr_cli_error("toolchain","expected list [--target TARGET], config-path, use --target TARGET PROVIDER, or reset [--target TARGET]");
        return XR_CLI_EXIT_USAGE;
    }
    XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(&limits,&resources);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        xr_cli_error("toolchain","preference resource owner creation failed");return XR_CLI_EXIT_UNAVAILABLE;
    }
    XrCompileState *state=NULL;opened=xr_compile_state_new(resources,&state);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_release(resources);
        xr_cli_error("toolchain","preference state creation failed");return XR_CLI_EXIT_UNAVAILABLE;
    }
    int result;
    if(list || path_only)result=toolchain_read(inv,state,path_only);
    else {
#ifdef XR_OS_WINDOWS
        result=toolchain_update(inv,state,reset);
#else
        xr_cli_error("toolchain","persistent preference updates are unavailable on this host");
        result=XR_CLI_EXIT_UNAVAILABLE;
#endif
    }
    xr_compile_state_release(state);xr_compile_resources_release(resources);return result;
}
