/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_graph_authority.c - Exact source graph authority for CLI commands
 */

#include "xcli_graph_authority.h"
#include "xcli_fs.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../os/os_fs.h"
#include "../../xir/xxir_library_catalog.h"

struct XrCliGraphAuthority {
    XrXirCompileContext context;
    XrProject *project;
    XrLockfile *lockfile;
    XrProjectAuthority *project_authority;
    XrModuleIdentityAuthority script_authority;
    char *script_root;
    char *entry_path;
    XrModuleOverlay *overlay;
    const XrXirLibraryCatalog *libraries;
};
static XrManifestStatus authority_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_MANIFEST_OK;
    case XR_OS_IO_NOT_FOUND:return XR_MANIFEST_NOT_FOUND;
    case XR_OS_IO_BUDGET:return XR_MANIFEST_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_MANIFEST_OUT_OF_MEMORY;
    case XR_OS_IO_BAD_ARGUMENT:return XR_MANIFEST_BAD_ARGUMENT;
    case XR_OS_IO_UNSUPPORTED:return XR_MANIFEST_UNSUPPORTED;
    default:return XR_MANIFEST_IO;
    }
}
static bool authority_absolute(XrIoContext *io, const char *path) {
    size_t length=0;if(!io_length(io,path,&length)||!length)return false;
    if(!io_work(io,length<3?length:3))return false;
    unsigned char first=(unsigned char)path[0],second=length>1?(unsigned char)path[1]:0,third=length>2?(unsigned char)path[2]:0;
#ifdef XR_OS_WINDOWS
    return (length>=2&&(first=='/'||first=='\\')&&(second=='/'||second=='\\')) ||
        (length>=3&&((first>='A'&&first<='Z')||(first>='a'&&first<='z'))&&
        second==':'&&(third=='/'||third=='\\'));
#else
    (void)second;(void)third;return first=='/';
#endif
}
static bool authority_equal(XrIoContext *io, const char *a, const char *b, bool *equal) {
    for(;;) {
        if(!io_work(io,2))return false;
        char x=*a++,y=*b++;if(x!=y||!x){*equal=x==y;return true;}
    }
}
XR_FUNC XrOsIoStatus xr_cli_find_project_root_owned(const XrOsIoPolicy *policy,
    const char *absolute_start_directory, char **output) {
    if(!io_policy_valid(policy)||!absolute_start_directory||!output||*output)return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};
    if(!authority_absolute(&io,absolute_start_directory))return io.status==XR_OS_IO_OK?XR_OS_IO_BAD_ARGUMENT:io.status;
    XrFsStat info={0};io_status(&io,xr_os_io_stat(policy,absolute_start_directory,&info));
    if(io.status!=XR_OS_IO_OK)return io.status;
    if(info.kind!=XR_FS_DIR)return XR_OS_IO_BAD_ARGUMENT;
    char *current=NULL;io_status(&io,xr_realpath_owned(policy,absolute_start_directory,&current));
    while(io.status==XR_OS_IO_OK) {
        char *candidate=NULL;io_status(&io,xr_path_join_owned(policy,current,"xray.toml",&candidate));
        if(io.status==XR_OS_IO_OK) {
            XrOsIoStatus probe=xr_file_probe_owned(policy,candidate,true);
            if(probe==XR_OS_IO_OK) {
                io_free(&io,candidate);
                if(io_work(&io,1)){*output=current;current=NULL;}
                break;
            }
            if(probe!=XR_OS_IO_NOT_FOUND)io_status(&io,probe);
        }
        io_free(&io,candidate);
        if(io.status!=XR_OS_IO_OK)break;
        char *parent=NULL;io_status(&io,xr_path_dirname_owned(policy,current,&parent));
        bool equal=false;
        if(io.status==XR_OS_IO_OK)authority_equal(&io,current,parent,&equal);
        if(io.status==XR_OS_IO_OK&&equal)io_status(&io,XR_OS_IO_NOT_FOUND);
        if(io.status==XR_OS_IO_OK){io_free(&io,current);current=parent;parent=NULL;}
        io_free(&io,parent);
    }
    io_free(&io,current);return io.status;
}
#ifdef XR_OS_WINDOWS
static bool authority_stem(XrIoContext *io,const char *stem,const char *reserved,size_t size) {
    for(size_t i=0;i<size;++i) {
        if(!io_work(io,2)||stem[i]!=reserved[i])return false;
    }
    return true;
}
#endif
/* The lexical tail never receives OS normalization: validate each component
 * before asking the OS about its nearest existing ancestor. */
static bool authority_entry_path(XrIoContext *io, const char *path) {
    size_t length = 0;
    if (!authority_absolute(io,path) || !io_length(io,path,&length)) return false;
    size_t start = 1;
#ifdef XR_OS_WINDOWS
    if (length >= 3 && path[1] == ':') start = 3;
    else {
        if (length < 5 || !((path[0] == '/' && path[1] == '/') ||
            (path[0] == '\\' && path[1] == '\\'))) return false;
        start = 2;
    }
#endif
    size_t components = 0;
    for (size_t i = start; i <= length; ++i) {
        if (!io_work(io,1)) return false;
        unsigned char c = (unsigned char)path[i];
        bool separator = c == '/';
#ifdef XR_OS_WINDOWS
        separator = separator || c == '\\';
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
#else
        if (c == '\\') return false;
#endif
        if (i < length && !separator) { if (c < 32 || c == 127) return false; continue; }
        size_t size = i - start;
        if (!size || (size == 1 && path[start] == '.') ||
            (size == 2 && path[start] == '.' && path[start+1] == '.')) return false;
#ifdef XR_OS_WINDOWS
        if (path[i-1] == '.' || path[i-1] == ' ') return false;
        /* Win32 device names (including names with an extension) are not a
         * source-module identity. Reject rather than guess OS aliasing. */
        char stem[4] = {0}; size_t stem_size = 0;
        while (stem_size < size && stem_size < sizeof(stem) && path[start+stem_size] != '.') {
            if (!io_work(io,1)) return false;
            unsigned char x = (unsigned char)path[start+stem_size];
            stem[stem_size++] = (char)(x >= 'a' && x <= 'z' ? x - 'a' + 'A' : x);
        }
        if ((stem_size == 3 && (authority_stem(io,stem,"CON",3) || authority_stem(io,stem,"PRN",3) ||
            authority_stem(io,stem,"AUX",3) || authority_stem(io,stem,"NUL",3))) ||
            (stem_size == 4 && (stem[3] >= '1' && stem[3] <= '9') &&
            (authority_stem(io,stem,"COM",3) || authority_stem(io,stem,"LPT",3)) &&
            (size == 4 || path[start+4] == '.'))) return false;
#endif
        ++components; start = i + 1;
    }
#ifdef XR_OS_WINDOWS
    if (path[1] != ':' && components < 3) return false;
#else
    (void)components;
#endif
    return true;
}
/* canonical_directory is a real existing directory. Missing suffix components
 * are retained verbatim after the existing path helper canonicalizes that parent.
 * This follows the existing OS path contract; it does not invent symlink equality. */
static XrOsIoStatus authority_text_entry(XrIoContext *io, const char *entry,
    char **canonical_entry, char **canonical_directory) {
    if (!authority_entry_path(io,entry))
        return io->status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : io->status;
    XrFsStat leaf = {0}; XrOsIoStatus probe = xr_os_io_stat(io->policy,entry,&leaf);
    if (probe == XR_OS_IO_OK && leaf.kind != XR_FS_FILE) return XR_OS_IO_BAD_ARGUMENT;
    if (probe != XR_OS_IO_OK && probe != XR_OS_IO_NOT_FOUND) return probe;
    char *directory = NULL, *canonical = NULL, *joined = NULL;
    io_status(io,xr_path_dirname_owned(io->policy,entry,&directory));
    while (io->status == XR_OS_IO_OK) {
        XrFsStat info = {0}; probe = xr_os_io_stat(io->policy,directory,&info);
        if (probe == XR_OS_IO_OK) {
            if (info.kind != XR_FS_DIR) io_status(io,XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        if (probe != XR_OS_IO_NOT_FOUND) { io_status(io,probe); break; }
        char *parent = NULL; io_status(io,xr_path_dirname_owned(io->policy,directory,&parent));
        bool same = false;
        if (io->status == XR_OS_IO_OK) authority_equal(io,parent,directory,&same);
        if (io->status == XR_OS_IO_OK && same) io_status(io,XR_OS_IO_NOT_FOUND);
        if (io->status == XR_OS_IO_OK) { io_free(io,directory); directory=parent; parent=NULL; }
        io_free(io,parent);
    }
    if (io->status == XR_OS_IO_OK) io_status(io,xr_realpath_owned(io->policy,directory,&canonical));
    size_t prefix = 0;
    if (io->status == XR_OS_IO_OK && io_length(io,directory,&prefix)) {
        const char *tail = entry + prefix;
        if (io_work(io,1) && (*tail == '/' || *tail == '\\')) ++tail;
        if (io->status == XR_OS_IO_OK) io_status(io,xr_path_join_owned(io->policy,canonical,tail,&joined));
    }
    if (io->status == XR_OS_IO_OK) {
        *canonical_entry=joined;joined=NULL; *canonical_directory=canonical;canonical=NULL;
    }
    io_free(io,joined);io_free(io,canonical);io_free(io,directory);return io->status;
}

XR_FUNC void xr_cli_compile_graph_authority_close(XrCliGraphAuthority *authority) {
    if(!authority)return;
    xr_project_authority_free_owned(authority->project_authority);
    xr_lockfile_free_owned(authority->lockfile);xr_project_free_owned(authority->project);
    xr_compile_module_overlay_free(authority->overlay);
    xr_compile_resources_free(authority->entry_path);
    xr_compile_resources_free(authority->script_root);xr_compile_resources_free(authority);
}
XR_FUNC XrManifestStatus xr_cli_compile_graph_authority_open_input(const XrXirCompileContext *context,
    const XrCliGraphEntryInput *input, const XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrCliGraphAuthority **output, XrManifestDiagnostic *diagnostic) {
    if(!context||!context->resources||!input||!input->absolute_path||!limits||!output||*output ||
        (input->kind != XR_CLI_GRAPH_ENTRY_FILE && input->kind != XR_CLI_GRAPH_ENTRY_TEXT) ||
        (input->kind == XR_CLI_GRAPH_ENTRY_FILE && (input->text || input->length)) ||
        (input->kind == XR_CLI_GRAPH_ENTRY_TEXT && !input->text)) return XR_MANIFEST_BAD_ARGUMENT;
    if(libraries&&xr_xir_compile_library_catalog_context(libraries)->resources!=context->resources)return XR_MANIFEST_BAD_ARGUMENT;
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);XrIoContext io={&policy,XR_OS_IO_OK};
    if(!authority_absolute(&io,input->absolute_path))return io.status==XR_OS_IO_OK?XR_MANIFEST_BAD_ARGUMENT:authority_status(io.status);
    XrCliGraphAuthority *authority=io_alloc(&io,sizeof(*authority));
    if(!authority)return authority_status(io.status);
    if(!io_clear(&io,authority,sizeof(*authority))){io_free(&io,authority);return authority_status(io.status);}
    authority->context=*context;authority->libraries=libraries;
    char *directory=NULL,*root=NULL,*lockpath=NULL,*identity=NULL,*logical=NULL;
    XrManifestStatus status=XR_MANIFEST_OK;
    if (input->kind == XR_CLI_GRAPH_ENTRY_TEXT) {
        io_status(&io,authority_text_entry(&io,input->absolute_path,&authority->entry_path,&directory));
    } else {
        io_status(&io,xr_file_probe_owned(&policy,input->absolute_path,true));
        if(io.status==XR_OS_IO_OK)io_status(&io,xr_realpath_owned(&policy,input->absolute_path,&authority->entry_path));
        if(io.status==XR_OS_IO_OK)io_status(&io,xr_path_dirname_owned(&policy,authority->entry_path,&directory));
    }
    if(io.status!=XR_OS_IO_OK) { status=authority_status(io.status); goto done; }
    XrOsIoStatus find=xr_cli_find_project_root_owned(&policy,directory,&root);
    status=authority_status(find);
    if(find==XR_OS_IO_OK) {
        status=xr_project_load_owned(&policy,root,limits,&authority->project,diagnostic);
        if(status==XR_MANIFEST_OK)status=xr_project_authority_build_owned(authority->project,&authority->project_authority,diagnostic);
        if(status==XR_MANIFEST_OK) {
            io_status(&io,xr_path_join_owned(&policy,root,"xray.lock",&lockpath));
            if(io.status==XR_OS_IO_OK) {
                XrOsIoStatus load=xr_lockfile_load_owned(&policy,lockpath,&authority->lockfile);
                if(load!=XR_OS_IO_NOT_FOUND)io_status(&io,load);
            }
            status=authority_status(io.status);
        }
    } else if(find==XR_OS_IO_NOT_FOUND) {
        io_status(&io,xr_path_dirname_owned(&policy,authority->entry_path,&authority->script_root));
        if(io.status==XR_OS_IO_OK) {
            authority->script_authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,authority->script_root};
        }
        status=authority_status(io.status);
    }
    if(status==XR_MANIFEST_OK) {
        const XrModuleIdentityAuthority *entry_authority=xr_cli_compile_graph_authority_entry(authority);
        XrModuleStatus module=xr_compile_module_identity_from_source(context->resources,entry_authority,
            authority->entry_path,&identity,&logical);
        status=module==XR_MODULE_OK?XR_MANIFEST_OK:module==XR_MODULE_BUDGET?XR_MANIFEST_BUDGET:
            module==XR_MODULE_OUT_OF_MEMORY?XR_MANIFEST_OUT_OF_MEMORY:XR_MANIFEST_INVALID;
        if(status==XR_MANIFEST_OK && input->kind==XR_CLI_GRAPH_ENTRY_TEXT) {
            XrModuleOverlayInput entry={*entry_authority,logical,authority->entry_path,input->text,input->length};
            module=xr_compile_module_overlay_new(context->resources,&entry,1,&authority->overlay);
            status=module==XR_MODULE_OK?XR_MANIFEST_OK:module==XR_MODULE_BUDGET?XR_MANIFEST_BUDGET:
                module==XR_MODULE_OUT_OF_MEMORY?XR_MANIFEST_OUT_OF_MEMORY:XR_MANIFEST_INVALID;
        }
    }
    if(status==XR_MANIFEST_OK) {
        if(io_work(&io,1)){*output=authority;authority=NULL;}
        else status=authority_status(io.status);
    }
done:
    io_free(&io,identity);io_free(&io,logical);io_free(&io,lockpath);io_free(&io,root);io_free(&io,directory);
    xr_cli_compile_graph_authority_close(authority);return status;
}
XR_FUNC XrManifestStatus xr_cli_compile_graph_authority_open(const XrXirCompileContext *context,
    const char *entry, const XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrCliGraphAuthority **output, XrManifestDiagnostic *diagnostic) {
    XrCliGraphEntryInput input={XR_CLI_GRAPH_ENTRY_FILE,entry,NULL,0};
    return xr_cli_compile_graph_authority_open_input(context,&input,libraries,limits,output,diagnostic);
}
XR_FUNC const char *xr_cli_compile_graph_authority_source_path(const XrCliGraphAuthority *authority) {
    return authority?authority->entry_path:NULL;
}
XR_FUNC const XrModuleOverlayInput *xr_cli_compile_graph_authority_overlay(const XrCliGraphAuthority *authority) {
    if(!authority || !authority->overlay)return NULL;
    size_t count=0;const XrModuleOverlayEntry *entries=xr_compile_module_overlay_entries(authority->overlay,&count);
    return count==1?&entries[0].source:NULL;
}
XR_FUNC const XrModuleIdentityAuthority *xr_cli_compile_graph_authority_entry(const XrCliGraphAuthority *authority) {
    return !authority?NULL:authority->project_authority?xr_project_authority_view(authority->project_authority):&authority->script_authority;
}
XR_FUNC const XrProject *xr_cli_compile_graph_authority_project(const XrCliGraphAuthority *authority) { return authority?authority->project:NULL; }
XR_FUNC XrLockfile *xr_cli_compile_graph_authority_lockfile(const XrCliGraphAuthority *authority) { return authority?authority->lockfile:NULL; }
XR_FUNC const XrXirLibraryCatalog *xr_cli_compile_graph_authority_catalog(const XrCliGraphAuthority *authority) { return authority?authority->libraries:NULL; }
XR_FUNC const XrXirCompileContext *xr_cli_compile_graph_authority_context(const XrCliGraphAuthority *authority) { return authority?&authority->context:NULL; }
