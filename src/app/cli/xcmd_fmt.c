/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_fmt.c - Owned syntax formatting and atomic file publication
 */
#include "xcli.h"
#include "xcli_canonical_source.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../frontend/format/xfmt.h"
#include "../../frontend/parser/xparse.h"
#include "../../os/os_fs.h"
#include "../../os/os_dir.h"
#include "../toolchain/xtc_xir_publication.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FmtWork {
    XrCompileResources *resources;
    XrCompileState *state;
    XrOsIoPolicy policy;
    XrFmtConfig config;
    size_t total, changed, errors;
    int result;
    bool check_only, verbose, stopped, published;
} FmtWork;
typedef struct FmtDirectory {
    struct FmtDirectory *parent;
    XrDirIter *iterator;
    char *path;
} FmtDirectory;

static void fmt_failure(FmtWork *work, int result, bool stop) {
    ++work->errors;
    if (result > work->result) work->result = result;
    work->stopped |= stop;
}
static void fmt_io_failure(FmtWork *work, const char *path, XrOsIoStatus status) {
    fprintf(stderr,"xray fmt: %s: I/O status=%u\n",path,(unsigned)status);
    bool internal = status == XR_OS_IO_OUT_OF_MEMORY || status == XR_OS_IO_BAD_ARGUMENT;
    fmt_failure(work,internal ? XR_CLI_EXIT_INTERNAL :
        status == XR_OS_IO_UNSUPPORTED ? XR_CLI_EXIT_UNAVAILABLE : XR_CLI_EXIT_FAIL,
        internal || status == XR_OS_IO_BUDGET);
}
static bool fmt_resource(FmtWork *work, XrCompileResourceStatus status, const char *path) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    fmt_io_failure(work,path,status == XR_COMPILE_RESOURCE_BUDGET ? XR_OS_IO_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_BAD_ARGUMENT);
    return false;
}
static int fmt_publication_exit(XtcXirPublicationStatus status) {
    if (status == XTC_XIR_PUBLICATION_OK) return XR_CLI_EXIT_OK;
    if (status == XTC_XIR_PUBLICATION_OUT_OF_MEMORY || status == XTC_XIR_PUBLICATION_INVALID)
        return XR_CLI_EXIT_INTERNAL;
    return status == XTC_XIR_PUBLICATION_UNSUPPORTED ? XR_CLI_EXIT_UNAVAILABLE : XR_CLI_EXIT_FAIL;
}
static bool fmt_publish(FmtWork *work, const char *path, const XrFmtOutput *output) {
    XtcXirPublication *owner = NULL;
    XtcXirPublicationRequest request = {path,{XR_PATH_MAX,16,output->length ? output->length : 1}};
    XtcXirPublicationStatus status = xtc_xir_publication_new(work->resources,&request,&owner);
    if (status == XTC_XIR_PUBLICATION_OK) status = xtc_xir_publication_write(owner,output->text,output->length);
    if (status == XTC_XIR_PUBLICATION_OK) status = xtc_xir_publication_commit(owner);
    bool okay = status == XTC_XIR_PUBLICATION_OK;
    if(okay)work->published=true;
    if (!okay) {
        const XtcXirPublicationDiagnostic *d = xtc_xir_publication_diagnostic(owner);
        fprintf(stderr,"xray fmt: %s: publication status=%u stage=%u published=%u os=%u\n",
            path,(unsigned)status,d?(unsigned)d->stage:0,d?(unsigned)d->published:0,d?d->os_error:0);
        fmt_failure(work,fmt_publication_exit(status),status == XTC_XIR_PUBLICATION_BUDGET ||
            status == XTC_XIR_PUBLICATION_OUT_OF_MEMORY || status == XTC_XIR_PUBLICATION_INVALID);
    }
    for (unsigned attempt=0;attempt<8;++attempt) {
        if (xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK) return okay;
        XtcXirPublicationDiagnostic d=*xtc_xir_publication_diagnostic(owner);
        fprintf(stderr,"xray fmt: %s: publication cleanup pending published=%u os=%u\n",
            path,(unsigned)d.published,d.cleanup_os_error);
        okay=false;
        fmt_failure(work,fmt_publication_exit(d.status),false);
    }
    fprintf(stderr,"xray fmt: terminal publication cleanup failure\n");
    fflush(stdout); fflush(stderr);
    _Exit(XR_CLI_EXIT_INTERNAL);
}

static void fmt_file(FmtWork *work, const char *path) {
    ++work->total;
    uint8_t *source=NULL; size_t length=0;
    XrCompilerSession *session=NULL; AstNode *ast=NULL; XrFmtOutput output={0};
    XrOsIoStatus io=xr_os_io_read_regular_file(&work->policy,path,SIZE_MAX-1,&source,&length);
    if(io!=XR_OS_IO_OK) { fmt_io_failure(work,path,io); goto cleanup; }
    for(size_t i=0;i<length;++i) {
        if(!fmt_resource(work,xr_compile_state_work(work->state,1),path)) goto cleanup;
        if(!source[i]) {
            fprintf(stderr,"xray fmt: %s: source contains a NUL byte\n",path);
            fmt_failure(work,XR_CLI_EXIT_FAIL,false); goto cleanup;
        }
    }
    void *terminated=source;
    XrCompileResourceStatus resized=xr_compile_resources_resize(work->resources,&terminated,length+1);
    source=terminated;
    if(!fmt_resource(work,resized,path) || !fmt_resource(work,xr_compile_state_work(work->state,1),path)) goto cleanup;
    source[length]=0;
    XrCompilerSessionStatus opened=xr_compile_session_new(work->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        fmt_resource(work,opened==XR_COMPILER_SESSION_BUDGET ? XR_COMPILE_RESOURCE_BUDGET :
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT,path);
        goto cleanup;
    }
    XrParseStatus parsed=xr_compile_parse_with_trivia(session,(const char *)source,path,NULL,&ast);
    if(parsed!=XR_PARSE_OK) {
        fprintf(stderr,"xray fmt: %s: parser status=%u\n",path,(unsigned)parsed);
        bool internal=parsed==XR_PARSE_BAD_ARGUMENT || parsed==XR_PARSE_OUT_OF_MEMORY;
        fmt_failure(work,internal ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL,internal || parsed==XR_PARSE_BUDGET);
        goto cleanup;
    }
    XrFmtStatus formatted=xr_compile_format_ast(xr_compile_session_compile_state(session),ast,&work->config,&output);
    if(formatted!=XR_FMT_OK) {
        fprintf(stderr,"xray fmt: %s: formatter status=%u\n",path,(unsigned)formatted);
        fmt_failure(work,formatted==XR_FMT_BUDGET ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL,true);
        goto cleanup;
    }
    xr_program_destroy(ast); ast=NULL;
    xr_compile_session_free(session); session=NULL;
    bool changed=length!=output.length;
    if(!changed) {
        if(!fmt_resource(work,xr_compile_state_work(work->state,length),path)) goto cleanup;
        changed=memcmp(source,output.text,length)!=0;
    }
    if(changed) {
        if(work->check_only) { ++work->changed; printf("Needs formatting: %s\n",path); }
        else if(fmt_publish(work,path,&output)) {
            ++work->changed;
            if(work->verbose) printf("Formatted: %s\n",path);
        }
    } else if(work->verbose) printf("Unchanged: %s\n",path);
cleanup:
    xr_program_destroy(ast); xr_compile_session_free(session);
    xr_compile_format_output_free(&output); xr_compile_resources_free(source);
}
static bool fmt_name_equal(FmtWork *work, const char *name, const char *word) {
    for(;;) {
        if(!fmt_resource(work,xr_compile_state_work(work->state,2),"directory entry")) return false;
        unsigned char a=(unsigned char)*name++, b=(unsigned char)*word++;
        if(a!=b) return false;
        if(!a) return true;
    }
}
static bool fmt_skip_directory(FmtWork *work,const char *name) {
    if(!fmt_resource(work,xr_compile_state_work(work->state,1),"directory entry")) return true;
    return name[0]=='.' || fmt_name_equal(work,name,"node_modules") || fmt_name_equal(work,name,"build") ||
        fmt_name_equal(work,name,"build-asan") || fmt_name_equal(work,name,"build-release");
}
static bool fmt_source_name(FmtWork *work,const char *name) {
    size_t length=0;
    if(!fmt_resource(work,xr_compile_state_string_length(work->state,name,&length),"directory entry")) return false;
    return length>=4 && fmt_name_equal(work,name+length-3,".xr");
}
static bool fmt_push_directory(FmtWork *work,FmtDirectory **top,char *path) {
    FmtDirectory *frame=NULL;
    XrCompileResourceStatus allocated=xr_compile_state_calloc(work->state,1,sizeof(*frame),(void **)&frame);
    if(!fmt_resource(work,allocated,path)) { xr_compile_resources_free(path); return false; }
    XrOsIoStatus status=xr_os_io_dir_open(&work->policy,path,&frame->iterator);
    if(status!=XR_OS_IO_OK) {
        fmt_io_failure(work,path,status); xr_compile_resources_free(frame); xr_compile_resources_free(path); return false;
    }
    frame->path=path; frame->parent=*top; *top=frame;
    return true;
}
static void fmt_pop_directory(FmtDirectory **top) {
    FmtDirectory *frame=*top; *top=frame->parent;
    xr_os_io_dir_close(frame->iterator); xr_compile_resources_free(frame->path); xr_compile_resources_free(frame);
}
static void fmt_directory(FmtWork *work,const char *path) {
    char *copy=NULL; FmtDirectory *top=NULL;
    if(!fmt_resource(work,xr_compile_state_strdup(work->state,path,&copy),path)) return;
    if(!fmt_push_directory(work,&top,copy)) return;
    while(top && !work->stopped) {
        XrDirEntry entry;
        XrOsIoStatus status=xr_os_io_dir_next(top->iterator,&entry);
        if(status!=XR_OS_IO_OK) {
            if(status!=XR_OS_IO_END) fmt_io_failure(work,top->path,status);
            fmt_pop_directory(&top); continue;
        }
        if(entry.is_dir ? fmt_skip_directory(work,entry.name) : !fmt_source_name(work,entry.name)) continue;
        if(work->stopped) break;
        char *child=NULL;
        status=xr_path_join_owned(&work->policy,top->path,entry.name,&child);
        if(status!=XR_OS_IO_OK) { fmt_io_failure(work,top->path,status); continue; }
        XrFsStat stat;
        status=xr_os_io_stat(&work->policy,child,&stat);
        if(status!=XR_OS_IO_OK) fmt_io_failure(work,child,status);
        else if(stat.kind==XR_FS_DIR) { fmt_push_directory(work,&top,child); child=NULL; }
        else if(stat.kind==XR_FS_FILE) fmt_file(work,child);
        else fmt_io_failure(work,child,XR_OS_IO_BAD_ARGUMENT);
        xr_compile_resources_free(child);
    }
    while(top) fmt_pop_directory(&top);
}

XR_FUNC int cmd_fmt(const XrCliInvocation *inv) {
    if(!inv || inv->positional_count<0 || (inv->positional_count && !inv->positionals) ||
        (inv->options.count && (!inv->options.spec || !inv->options.present))) return XR_CLI_EXIT_INTERNAL;
    XrFmtConfig config=xfmt_default_config;
    bool check_only=xr_cli_opt_bool(&inv->options,"check"), verbose=xr_cli_opt_bool(&inv->options,"verbose");
    if (xr_cli_opt_bool(&inv->options, "tabs")) {
        config.use_tabs = 1;
    }
    int indent = xr_cli_opt_int(&inv->options, "indent", 0);
    if (indent > 0) {
        if (indent < 1 || indent > 16) {
            xr_cli_error("fmt", "invalid indent size %d (expected 1-16)", indent);
            return XR_CLI_EXIT_USAGE;
        }
        config.indent_size = indent;
    }
    int line_length = xr_cli_opt_int(&inv->options, "line-length", 0);
    if (line_length > 0) {
        if (line_length < 40 || line_length > 400) {
            xr_cli_error("fmt", "invalid line length %d (expected 40-400)", line_length);
            return XR_CLI_EXIT_USAGE;
        }
        config.max_line_length = line_length;
    }
    if (xr_cli_opt_bool(&inv->options, "align-branch-arrows")) {
        config.align_branch_arrows = 1;
    }
    if (xr_cli_opt_bool(&inv->options, "no-align-branch-arrows")) {
        config.align_branch_arrows = 0;
    }
    if (xr_cli_opt_bool(&inv->options, "align-enum")) {
        config.align_enum_values = 1;
    }
    if (xr_cli_opt_bool(&inv->options, "align-fields")) {
        config.align_struct_fields = 1;
    }
    if (xr_cli_opt_bool(&inv->options, "align-comments")) {
        config.align_trailing_comments = 1;
    }
    if (xr_cli_opt_bool(&inv->options, "wrap")) {
        config.wrap_long_lines = 1;
    }
    if (xr_cli_opt_bool(&inv->options, "no-trailing-comma")) {
        config.multiline_trailing_comma = 0;
    }


    FmtWork work={0}; work.config=config; work.check_only=check_only; work.verbose=verbose;
    XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    XrCompileResourceStatus status=xr_compile_resources_new(&limits,&work.resources);
    if(!fmt_resource(&work,status,"request")) return work.result;
    work.policy=xr_compile_io_policy(work.resources);
    status=xr_compile_state_new(work.resources,&work.state);
    if(!fmt_resource(&work,status,"request")) goto cleanup;
    if(!inv->positional_count) fmt_directory(&work,".");
    for(int i=0;i<inv->positional_count && !work.stopped;++i) {
        const char *path=inv->positionals[i];
        XrFsStat stat;
        XrOsIoStatus io=xr_os_io_stat(&work.policy,path,&stat);
        if(io!=XR_OS_IO_OK) fmt_io_failure(&work,path ? path : "<null>",io);
        else if(stat.kind==XR_FS_DIR) fmt_directory(&work,path);
        else if(stat.kind==XR_FS_FILE) fmt_file(&work,path);
        else fmt_io_failure(&work,path,XR_OS_IO_BAD_ARGUMENT);
    }
    if(work.total) {
        if(check_only && work.changed) printf("\n%zu files need formatting (of %zu)\n",work.changed,work.total);
        else if(check_only && !work.errors) printf("\nOK: all %zu files properly formatted\n",work.total);
        else if(!check_only) printf("\nFormatted %zu files (of %zu)\n",work.changed,work.total);
    }
    if(check_only && work.changed && !work.result) work.result=XR_CLI_EXIT_FAIL;
cleanup:
    xr_compile_state_release(work.state); xr_compile_resources_release(work.resources);
    int output_flush=fflush(stdout), error_flush=fflush(stderr);
    if(output_flush || error_flush || ferror(stdout) || ferror(stderr)) {
        fprintf(stderr,"xray fmt: output failure published=%u\n",(unsigned)work.published);
        if(work.result<XR_CLI_EXIT_FAIL) work.result=XR_CLI_EXIT_FAIL;
    }
    return work.result;
}
