/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain_update.inc.c - Retained transaction cleanup for one-shot commands
 */
static int toolchain_update_error(XrCliToolchainUpdateStatus status) {
    const char *reason=status==XR_CLI_TOOLCHAIN_UPDATE_INVALID ? "invalid preference request or configuration" :
        status==XR_CLI_TOOLCHAIN_UPDATE_BUDGET ? "preference local limit exceeded" :
        status==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY ? "preference allocation failed" :
        status==XR_CLI_TOOLCHAIN_UPDATE_MEMORY_LIMIT ? "preference memory limit exceeded" :
        status==XR_CLI_TOOLCHAIN_UPDATE_OVERFLOW ? "preference size overflow" :
        status==XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED ? "preference path is unresolved" :
        status==XR_CLI_TOOLCHAIN_UPDATE_UNSUPPORTED ? "preference operation is unsupported" : "preference I/O failed";
    xr_cli_error("toolchain","%s",reason);return XR_CLI_EXIT_FAIL;
}
static int toolchain_close(XrCliToolchainUpdate **owner) {
    bool failed=false;XrCliToolchainUpdateView last={0};
    for(unsigned attempt=0;attempt<8;++attempt) {
        if(xr_cli_toolchain_update_close(owner)==XR_CLI_TOOLCHAIN_UPDATE_OK)
            return failed ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
        failed=true;last=*xr_cli_toolchain_update_view(*owner);
        xr_cli_error("toolchain","preference cleanup pending (published=%u removed=%u first_error=%u)",
            (unsigned)last.published,(unsigned)last.removed,(unsigned)last.first_error);
    }
    /* The command cannot discard a lock/publication owner still awaiting OS
     * cleanup. Terminal process exit releases the unrecoverable OS handles. */
    fflush(stdout);fflush(stderr);_Exit(XR_CLI_EXIT_INTERNAL);
}
static int toolchain_update(const XrCliInvocation *inv,XrCompileState *state,bool reset) {
    XrCliToolchainUpdateRequest request={.target=xr_cli_opt_string(&inv->options,"target",NULL),.reset=reset};
    if(!reset) {
        char error[256];
        if(!*inv->positionals[1]){xr_cli_error("toolchain","preference provider is empty");return XR_CLI_EXIT_USAGE;}
        if(!xtc_selector_parse(inv->positionals[1],&request.selector,error,sizeof(error))) {
            xr_cli_error("toolchain","%s",error);return XR_CLI_EXIT_USAGE;
        }
        request.compiler=xr_cli_opt_string(&inv->options,"cc",NULL);
        request.zig=xr_cli_opt_string(&inv->options,"zig",NULL);
    }
    XrCliToolchainUpdate *owner=NULL;
    XrCliToolchainUpdateStatus status=xr_cli_toolchain_update_new(state,&request,&owner);
    bool request_rejected=status==XR_CLI_TOOLCHAIN_UPDATE_INVALID;
    if(status==XR_CLI_TOOLCHAIN_UPDATE_OK)status=xr_cli_toolchain_update_execute(owner);
    int result=status==XR_CLI_TOOLCHAIN_UPDATE_OK ? XR_CLI_EXIT_OK : toolchain_update_error(status);
    if(request_rejected)result=XR_CLI_EXIT_USAGE;
    if(result==XR_CLI_EXIT_OK) {
        const XrCliToolchainUpdateView *view=xr_cli_toolchain_update_view(owner);
        if(toolchain_json(inv)) {
            fputs(reset ? "{\"schema\":1,\"reset\":true,\"target\":" : "{\"schema\":1,\"updated\":true,\"target\":",stdout);
            toolchain_string(request.target);fputs(",\"configPath\":",stdout);toolchain_string(view->path);
            if(!reset){fputs(",\"provider\":",stdout);toolchain_string(xtc_selector_name(request.selector));}
            puts("}");
        } else printf("Toolchain preference %s: %s\n",reset ? "reset" : "updated",request.target ? request.target : "all");
        result=toolchain_output_status();
    }
    int cleanup=toolchain_close(&owner);return result==XR_CLI_EXIT_OK ? cleanup : result;
}
