/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compiler.c - One resource ledger and parser owner across source builds
 *
 * KEY CONCEPT:
 *   Owned products are published only after ordinary public admission.
 */
#include "xsource_compiler.h"
#include "xsource_authority.h"
#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include "../os/os_fs.h"
#include "../xir/xxir_compile_memory.h"
#include "../xir/xxir_stdlib_internal.h"
#include "../frontend/parser/xast.h"
#include "../toolchain/xcompiler_arena_backing.h"

struct XrSourceCompiler {
    XrXirCompileContext context;
    XrXirTarget target;
    XrTomlParseLimits manifest_limits;
    XrCompilerSession *session;
    XrXirLibraryCatalog *libraries;
    char *stdlib_path;
    bool borrowed_catalog;
};
static XrSourceCompileStatus compiler_resource(XrCompileResourceStatus status) {
    switch(status) {
    case XR_COMPILE_RESOURCE_OK:return XR_SOURCE_COMPILE_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT:return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OVERFLOW:return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY:return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_CANCELLED:return XR_SOURCE_COMPILE_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE:return XR_SOURCE_COMPILE_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE:return XR_SOURCE_COMPILE_CLOCK_FAILURE;
    default:return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    }
}
static XrSourceCompileStatus compiler_io(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_SOURCE_COMPILE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_OS_IO_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_OS_IO_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_OS_IO_CANCELLED: return XR_SOURCE_COMPILE_CANCELLED;
    case XR_OS_IO_DEADLINE: return XR_SOURCE_COMPILE_DEADLINE;
    case XR_OS_IO_CLOCK_FAILURE: return XR_SOURCE_COMPILE_CLOCK_FAILURE;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_OS_IO_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_IO;
    }
}
static XrSourceCompileStatus compiler_xir(XrXirStatus status) {
    switch (status) {
    case XR_XIR_OK: return XR_SOURCE_COMPILE_OK;
    case XR_XIR_BAD_STRUCTURE: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_XIR_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_XIR_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_XIR_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_XIR_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_XIR_CANCELLED: return XR_SOURCE_COMPILE_CANCELLED;
    case XR_XIR_DEADLINE: return XR_SOURCE_COMPILE_DEADLINE;
    case XR_XIR_CLOCK_FAILURE: return XR_SOURCE_COMPILE_CLOCK_FAILURE;
    case XR_XIR_IO: return XR_SOURCE_COMPILE_IO;
    case XR_XIR_UNRESOLVED: return XR_SOURCE_COMPILE_NOT_FOUND;
    default: return XR_SOURCE_COMPILE_REJECTED;
    }
}
static XrSourceCompileStatus compiler_manifest(XrManifestStatus status) {
    switch (status) {
    case XR_MANIFEST_OK: return XR_SOURCE_COMPILE_OK;
    case XR_MANIFEST_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_MANIFEST_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_MANIFEST_LIMIT: return XR_SOURCE_COMPILE_LIMIT;
    case XR_MANIFEST_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_MANIFEST_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_MANIFEST_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_MANIFEST_IO: return XR_SOURCE_COMPILE_IO;
    case XR_MANIFEST_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_INVALID;
    }
}
static bool compiler_diagnostic_empty(const XrSourceCompileDiagnostic *diagnostic) {
    return !diagnostic || (!diagnostic->source.snapshot && xr_xir_compile_source_failure_empty(&diagnostic->source.failure));
}
static XrSourceCompileStatus compiler_publish(XrSourceCompileDiagnostic *out,
    XrSourceCompileDiagnostic *detail, XrSourceCompileStatus status) {
    detail->status = status;
    if (out) *out = *detail;
    else xr_source_compile_diagnostic_free(detail);
    return status;
}
XR_FUNC void xr_source_compiler_free(XrSourceCompiler *compiler) {
    if (!compiler) return;
    if (!compiler->borrowed_catalog) xr_xir_compile_library_catalog_free(compiler->libraries);
    xr_compile_session_free(compiler->session);
    if (!compiler->borrowed_catalog) xr_compile_resources_free(compiler->stdlib_path);
    xr_compile_resources_free(compiler);
}
static bool compiler_absolute(XrIoContext *io, const char *path) {
    unsigned char prefix[3] = {0};
    for (size_t i = 0; i < sizeof(prefix); ++i) {
        if (!((io)->status == XR_OS_IO_OK)) return false;
        prefix[i] = (unsigned char)path[i]; if (!prefix[i]) break;
    }
#ifdef XR_OS_WINDOWS
    bool sep0 = prefix[0] == '/' || prefix[0] == '\\';
    bool sep1 = prefix[1] == '/' || prefix[1] == '\\';
    bool letter = (prefix[0] >= 'A' && prefix[0] <= 'Z') || (prefix[0] >= 'a' && prefix[0] <= 'z');
    return (sep0 && sep1) || (letter && prefix[1] == ':' && (prefix[2] == '/' || prefix[2] == '\\'));
#else
    return prefix[0] == '/';
#endif
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_new(const XrSourceCompilerConfig *config,
    XrCompileRequestControl *control,XrSourceCompiler **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};
    if (!config || !output || *output || !xir_compile_context_valid(&config->context) ||
        !config->absolute_stdlib_path || !config->manifest_limits.input_bytes ||
        !config->manifest_limits.depth || config->manifest_limits.depth > 128)
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_BAD_ARGUMENT);
    if (config->target.architecture != XR_XIR_ARCH_X86_64 || config->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_UNSUPPORTED);
    XrOsIoPolicy input_policy = xr_compile_io_policy(config->context.resources);
    XrIoContext input_io = {&input_policy, XR_OS_IO_OK};
    detail.stage = XR_SOURCE_COMPILE_PATH;
    if (!compiler_absolute(&input_io, config->absolute_stdlib_path))
        return compiler_publish(diagnostic, &detail, input_io.status == XR_OS_IO_OK ?
            XR_SOURCE_COMPILE_BAD_ARGUMENT : compiler_io(input_io.status));
    XrSourceCompiler *compiler = NULL;
    XrSourceCompileStatus status = compiler_resource(xr_compile_resources_calloc(config->context.resources,
        1, sizeof(*compiler), (void **)&compiler));
    if (status != XR_SOURCE_COMPILE_OK) return compiler_publish(diagnostic, &detail, status);
    compiler->context = config->context; compiler->target = config->target;
    compiler->manifest_limits = config->manifest_limits;
    detail.stage = XR_SOURCE_COMPILE_SESSION;
    detail.session_status = xr_compile_session_new(config->context.resources, &compiler->session);
    if(detail.session_status==XR_COMPILE_RESOURCE_OK && control)
        detail.session_status=xr_compile_session_bind_request(compiler->session,control);
    if (detail.session_status != XR_COMPILE_RESOURCE_OK) {
        status=compiler_resource(detail.session_status);
    } else {
        detail.stage = XR_SOURCE_COMPILE_PATH;
        XrOsIoPolicy policy = xr_compile_io_policy(config->context.resources);
        XrOsIoStatus located = xr_realpath_owned(&policy, config->absolute_stdlib_path, &compiler->stdlib_path);
        XrFsStat info = {0};
        if (located == XR_OS_IO_OK) located = xr_os_io_stat(&policy, compiler->stdlib_path, &info);
        if (located == XR_OS_IO_OK && info.kind != XR_FS_DIR) located = XR_OS_IO_BAD_ARGUMENT;
        status = compiler_io(located);
        if (status == XR_SOURCE_COMPILE_OK) {
            detail.stage = XR_SOURCE_COMPILE_LIBRARY;
            status = compiler_xir(xir_stdlib_library_catalog_new(&compiler->context, compiler->stdlib_path, &compiler->libraries));
        }
    }
    if (status == XR_SOURCE_COMPILE_OK) *output = compiler;
    else xr_source_compiler_free(compiler);
    return compiler_publish(diagnostic, &detail, status);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_take_catalog(XrSourceCompiler **owner,
    XrSourceCompilerConfig *config,XrXirLibraryCatalog **libraries,char **stdlib_path) {
    if (!owner || !*owner || !config || !libraries || *libraries || !stdlib_path || *stdlib_path)
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompiler *compiler=*owner;
    if (compiler->borrowed_catalog || !compiler->libraries || !compiler->stdlib_path)
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    *config=(XrSourceCompilerConfig){compiler->context,compiler->target,compiler->stdlib_path,compiler->manifest_limits};
    *libraries=compiler->libraries;*stdlib_path=compiler->stdlib_path;
    compiler->libraries=NULL;compiler->stdlib_path=NULL;
    xr_source_compiler_free(compiler);*owner=NULL;return XR_SOURCE_COMPILE_OK;
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_catalog_request(const XrSourceCompilerConfig *config,
    XrXirLibraryCatalog *libraries,XrCompileRequestControl *control,XrSourceCompiler **output) {
    if (!config || !libraries || !output || *output || !xir_compile_context_valid(&config->context) ||
        !config->absolute_stdlib_path || !*config->absolute_stdlib_path ||
        xr_xir_compile_library_catalog_context(libraries)->resources!=config->context.resources)
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompiler *compiler=NULL;
    XrSourceCompileStatus status=compiler_resource(xr_compile_resources_calloc(config->context.resources,
        1,sizeof(*compiler),(void **)&compiler));
    if (status!=XR_SOURCE_COMPILE_OK)return status;
    compiler->context=config->context;compiler->target=config->target;
    compiler->manifest_limits=config->manifest_limits;compiler->libraries=libraries;
    compiler->stdlib_path=(char *)config->absolute_stdlib_path;compiler->borrowed_catalog=true;
    XrCompileResourceStatus opened=xr_compile_session_new(config->context.resources,&compiler->session);
    if(opened==XR_COMPILE_RESOURCE_OK && control)opened=xr_compile_session_bind_request(compiler->session,control);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        status=compiler_resource(opened);
        xr_source_compiler_free(compiler);return status;
    }
    *output=compiler;return XR_SOURCE_COMPILE_OK;
}
XR_FUNC const XrXirCompileContext *xr_source_compiler_context(const XrSourceCompiler *compiler) {
    return compiler ? &compiler->context : NULL;
}
XR_FUNC XrCompilerSession *xr_source_compiler_session(const XrSourceCompiler *compiler) {
    return compiler ? compiler->session : NULL;
}
XR_FUNC XrCompileState *xr_source_compiler_state(const XrSourceCompiler *compiler) {
    return compiler ? xr_compile_session_compile_state(compiler->session) : NULL;
}
XR_FUNC XrCompileResources *xr_source_compiler_resources(const XrSourceCompiler *compiler) {
    return compiler ? compiler->context.resources : NULL;
}
XR_FUNC XrXirTarget xr_source_compiler_target(const XrSourceCompiler *compiler) {
    return compiler ? compiler->target : (XrXirTarget){0};
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_generation_config(const XrSourceCompiler *compiler,
    const char *namespace_id,XrXirSourceGenerationConfig *output) {
    if(!compiler || !namespace_id || !output)return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    *output=(XrXirSourceGenerationConfig){compiler->context,namespace_id,compiler->stdlib_path,
        NULL,compiler->libraries};
    return XR_SOURCE_COMPILE_OK;
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_open_input(XrSourceCompiler *compiler,const XrSourceEntryInput *input,
    XrSourceAuthority **output,XrSourceCompileDiagnostic *diagnostic) {
    if(!compiler || !input || !output || *output || !compiler_diagnostic_empty(diagnostic))
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail={0};detail.stage=XR_SOURCE_COMPILE_AUTHORITY;
    XrSourceCompileStatus status=compiler_resource(xr_compile_session_resource_status(compiler->session));
    if(status==XR_SOURCE_COMPILE_OK) {
        XrXirSourceDiagnostic immediate={0};
        XrManifestDiagnostic authority_fact={0};
        const XrSourceAuthorityDiagnostics receivers={&authority_fact,&immediate,&detail.source.failure};
        detail.authority_status=xr_source_authority_open_input(&compiler->context,input,compiler->libraries,
            &compiler->manifest_limits,output,&receivers);
        (void)xr_source_compile_manifest_report(&detail,compiler->context.resources,&authority_fact);
        detail.source.source=(XrXirSourceDiagnosticFact){immediate.status,immediate.module};
        if(immediate.status!=XR_XIR_OK)(void)xr_xir_compile_source_failure_record(compiler->context.resources,
            &detail.source.failure,&immediate,detail.source.failure.path,input->text,input->length);
        status=compiler_manifest(detail.authority_status);
    }
    return compiler_publish(diagnostic,&detail,status);
}
XR_FUNC void xr_source_compiler_file_clear(XrSourceCompilerFile *file) {
    if(!file)return;
    xr_source_authority_close(file->authority);xr_compile_resources_free(file->bytes);
    *file=(XrSourceCompilerFile){0};
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_read_file(XrSourceCompiler *compiler,const char *path,
    XrSourceCompilerFile *output,XrSourceCompileDiagnostic *diagnostic) {
    if(!compiler || !path || !output || output->authority || output->bytes || output->length ||
        !compiler_diagnostic_empty(diagnostic))return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail={0};XrSourceCompilerFile file={0};char *absolute=NULL;
    XrOsIoPolicy policy=xr_compile_io_policy(compiler->context.resources);
    detail.stage=XR_SOURCE_COMPILE_PATH;
    XrSourceCompileStatus status=compiler_io(xr_realpath_owned(&policy,path,&absolute));
    const XrSourceEntryInput input={XR_SOURCE_ENTRY_FILE,absolute,NULL,0};
    if(status==XR_SOURCE_COMPILE_OK)status=xr_source_compiler_open_input(compiler,&input,&file.authority,&detail);
    xr_compile_resources_free(absolute);
    if(status==XR_SOURCE_COMPILE_OK) {
        detail.stage=XR_SOURCE_COMPILE_INPUT;
        status=compiler_io(xr_os_io_read_regular_file(&policy,xr_source_authority_source_path(file.authority),
            SIZE_MAX-1,&file.bytes,&file.length));
    }
    if(status==XR_SOURCE_COMPILE_OK)*output=file;
    else xr_source_compiler_file_clear(&file);
    return compiler_publish(diagnostic,&detail,status);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_file(XrSourceCompiler *compiler, const char *absolute_path,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler || !compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileStatus available = compiler_resource(xr_compile_session_resource_status(compiler->session));
    if (available != XR_SOURCE_COMPILE_OK) {
        XrSourceCompileDiagnostic detail = {0}; detail.stage = XR_SOURCE_COMPILE_SESSION;
        return compiler_publish(diagnostic, &detail, available);
    }
    XrSourceCompileRequest request = {&compiler->context, absolute_path, compiler->stdlib_path,
        compiler->libraries, compiler->manifest_limits, compiler->target, compiler->session};
    return xr_source_compile_build(&request, output, diagnostic);
}
typedef struct CompilerSuppliedInput { XrSourceCompilerText source; struct AstNode *program; bool ast; } CompilerSuppliedInput;
static XrSourceCompileStatus compiler_build_supplied(XrSourceCompiler *compiler, CompilerSuppliedInput supplied,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};
    const XrSourceCompilerText *input = &supplied.source;
    if (!compiler || (supplied.ast ? !supplied.program : !input->text) || !output || *output ||
        (input->absolute_path ? input->namespace_id != NULL : !input->namespace_id))
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_BAD_ARGUMENT);
    XrSourceCompileStatus status = compiler_resource(xr_compile_session_resource_status(compiler->session));
    if (status != XR_SOURCE_COMPILE_OK) return compiler_publish(diagnostic, &detail, status);
    if (supplied.ast) {
        if (supplied.program->type!=AST_PROGRAM || !supplied.program->as.program.owns_arena)
            return compiler_publish(diagnostic,&detail,XR_SOURCE_COMPILE_BAD_ARGUMENT);
        XrCompileState *syntax=xr_compiler_arena_compile_state(supplied.program->as.program.arena);
        if (!syntax || (syntax!=xr_source_compiler_state(compiler) && xr_compile_state_has_sessions(syntax)))
            return compiler_publish(diagnostic,&detail,XR_SOURCE_COMPILE_BAD_ARGUMENT);
        /* Detached syntax is immutable borrowed input. Checking and the ready
         * projection allocate into this request, never into the input arena. */
    }
    XrSourceAuthority *authority = NULL;
    XrModuleIdentityAuthority memory = {XR_MODULE_IDENTITY_MEMORY, input->namespace_id, NULL};
    const XrModuleIdentityAuthority *identity = &memory;
    XrXirSourceText text = {NULL, input->text, input->length};
    XrXirSourceAst ast = {text,supplied.program};
    const char *entry = NULL; XrLockfile *lockfile = NULL;
    if (input->absolute_path) {
        detail.stage = XR_SOURCE_COMPILE_AUTHORITY;
        XrSourceEntryInput rooted = {supplied.ast ? XR_SOURCE_ENTRY_AST : XR_SOURCE_ENTRY_TEXT,
            input->absolute_path, supplied.ast ? NULL : input->text, supplied.ast ? 0 : input->length};
        status = xr_source_compiler_open_input(compiler, &rooted, &authority, &detail);
        if (status == XR_SOURCE_COMPILE_OK) {
            identity = xr_source_authority_entry(authority); entry = xr_source_authority_source_path(authority);
            lockfile = xr_source_authority_lockfile(authority);
            if (supplied.ast) ast.source.logical_path = xr_source_authority_logical_path(authority);
            else {
                const XrModuleOverlayInput *overlay = xr_source_authority_overlay(authority);
                text = (XrXirSourceText){overlay->logical_path, overlay->text, overlay->length};
            }
        }
    }
    XrXirSourceProduct *product = NULL;
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_PRODUCT;
        XrXirSourceProductRequest request = {{compiler->session, entry, identity, &compiler->context,
            compiler->stdlib_path, lockfile, XR_XIR_PROGRAM, compiler->libraries}, compiler->target};
        status = compiler_xir(supplied.ast ?
            xr_xir_compile_source_product_build_ast(&request, &ast, &product, &detail.source) :
            xr_xir_compile_source_product_build_text(&request, &text, &product, &detail.source));
        if(detail.source.status==XR_XIR_BAD_STRUCTURE &&
            (detail.source.failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE ||
             detail.source.failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT))status=XR_SOURCE_COMPILE_INVALID;
        if (status == XR_SOURCE_COMPILE_OK) {
            XrXirSourceDiagnostic immediate={0};
            XrXirStatus admitted = xr_xir_compile_source_product_public_admit(product, &immediate);
            detail.source.source=(XrXirSourceDiagnosticFact){immediate.status,immediate.module};
            if(admitted!=XR_XIR_OK)(void)xr_xir_compile_source_failure_record(compiler->context.resources,
                &detail.source.failure,&immediate,NULL,NULL,0);
            status = compiler_xir(admitted);
            if (admitted != XR_XIR_OK) {
                detail.source.stage = XR_XIR_SOURCE_PRODUCT_FACTS; detail.source.status = admitted;
                xr_xir_compile_source_product_free(product); product = NULL;
            }
        }
    }
    xr_source_authority_close(authority);
    if (status == XR_SOURCE_COMPILE_OK) *output = product;
    return compiler_publish(diagnostic, &detail, status);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_text(XrSourceCompiler *compiler, const XrSourceCompilerText *input,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    return compiler_build_supplied(compiler,(CompilerSuppliedInput){input ? *input : (XrSourceCompilerText){0},NULL,false},
        output,diagnostic);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_ast(XrSourceCompiler *compiler, const XrSourceCompilerText *input,
    struct AstNode *program, XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!program) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    return compiler_build_supplied(compiler,(CompilerSuppliedInput){input ? *input : (XrSourceCompilerText){0},program,true},output,diagnostic);
}
