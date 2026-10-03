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
XR_FUNC void xr_cli_compile_graph_authority_close(XrCliGraphAuthority *authority) {
    if(!authority)return;
    xr_project_authority_free_owned(authority->project_authority);
    xr_lockfile_free_owned(authority->lockfile);xr_project_free_owned(authority->project);
    xr_compile_resources_free(authority->script_root);xr_compile_resources_free(authority);
}
XR_FUNC XrManifestStatus xr_cli_compile_graph_authority_open(const XrXirCompileContext *context,
    const char *entry, const XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrCliGraphAuthority **output, XrManifestDiagnostic *diagnostic) {
    if(!context||!context->resources||!entry||!limits||!output||*output)return XR_MANIFEST_BAD_ARGUMENT;
    if(libraries&&xr_xir_compile_library_catalog_context(libraries)->resources!=context->resources)return XR_MANIFEST_BAD_ARGUMENT;
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);XrIoContext io={&policy,XR_OS_IO_OK};
    if(!authority_absolute(&io,entry))return io.status==XR_OS_IO_OK?XR_MANIFEST_BAD_ARGUMENT:authority_status(io.status);
    XrCliGraphAuthority *authority=io_alloc(&io,sizeof(*authority));
    if(!authority)return authority_status(io.status);
    if(!io_clear(&io,authority,sizeof(*authority))){io_free(&io,authority);return authority_status(io.status);}
    authority->context=*context;authority->libraries=libraries;
    char *directory=NULL,*root=NULL,*lockpath=NULL;
    io_status(&io,xr_file_probe_owned(&policy,entry,true));
    if(io.status==XR_OS_IO_OK)io_status(&io,xr_path_dirname_owned(&policy,entry,&directory));
    if(io.status!=XR_OS_IO_OK) {
        XrManifestStatus failed=authority_status(io.status);
        io_free(&io,directory);xr_cli_compile_graph_authority_close(authority);return failed;
    }
    XrOsIoStatus find=xr_cli_find_project_root_owned(&policy,directory,&root);
    XrManifestStatus status=authority_status(find);
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
        XrModuleStatus module=xr_compile_module_identity_script_authority_from_source(context->resources,entry,&authority->script_authority,&authority->script_root);
        status=module==XR_MODULE_OK?XR_MANIFEST_OK:module==XR_MODULE_OUT_OF_MEMORY?XR_MANIFEST_OUT_OF_MEMORY:
            module==XR_MODULE_BUDGET?XR_MANIFEST_BUDGET:module==XR_MODULE_NOT_FOUND?XR_MANIFEST_NOT_FOUND:
            module==XR_MODULE_IO?XR_MANIFEST_IO:XR_MANIFEST_INVALID;
    }
    io_free(&io,lockpath);io_free(&io,root);io_free(&io,directory);
    if(status==XR_MANIFEST_OK) {
        if(io_work(&io,1)){*output=authority;authority=NULL;}
        else status=authority_status(io.status);
    }
    xr_cli_compile_graph_authority_close(authority);return status;
}
XR_FUNC const XrModuleIdentityAuthority *xr_cli_compile_graph_authority_entry(const XrCliGraphAuthority *authority) {
    return !authority?NULL:authority->project_authority?xr_project_authority_view(authority->project_authority):&authority->script_authority;
}
XR_FUNC const XrProject *xr_cli_compile_graph_authority_project(const XrCliGraphAuthority *authority) { return authority?authority->project:NULL; }
XR_FUNC XrLockfile *xr_cli_compile_graph_authority_lockfile(const XrCliGraphAuthority *authority) { return authority?authority->lockfile:NULL; }
XR_FUNC const XrXirLibraryCatalog *xr_cli_compile_graph_authority_catalog(const XrCliGraphAuthority *authority) { return authority?authority->libraries:NULL; }
XR_FUNC const XrXirCompileContext *xr_cli_compile_graph_authority_context(const XrCliGraphAuthority *authority) { return authority?&authority->context:NULL; }
