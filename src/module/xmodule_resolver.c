/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_resolver.c - Typed module resolution under one compiler resource owner
 */
#include "xmodule_resolver.h"
#include "xmodule_compile_internal.h"
#include "xstdlib_embedded.h"
#include "xlockfile.h"
#include "xsemver.h"
#include "../xir/xxir_library_catalog.h"
#include "../os/os_fs.h"
#include <ctype.h>
#include <stdlib.h>

XR_FUNC void xr_compile_module_id_cleanup(XrModuleId *id) {
    if (!id) return;
    xr_compile_resources_free(id->canonical); xr_compile_resources_free(id->logical_path);
    xr_compile_resources_free(id->source_path);
    xr_compile_resources_free((char *)id->authority.namespace_id);
    xr_compile_resources_free((char *)id->authority.physical_root);
    *id = (XrModuleId){0};
}
static void free_cached_entry(const char *key, void *value, void *unused) {
    (void)unused; xr_compile_resources_free((char *)key);
    xr_compile_module_id_cleanup(value); xr_compile_resources_free(value);
}
XR_FUNC void xr_compile_module_resolver_free(XrModuleResolver *resolver) {
    if (!resolver) return;
    xr_hashmap_owned_dispose(resolver->cache,free_cached_entry,NULL);
    xr_compile_resources_free((char *)resolver->config.stdlib_path);
    xr_compile_resources_free(resolver);
}
XR_FUNC XrModuleStatus xr_compile_module_resolver_new(XrCompileResources *resources,
    const XrModuleResolverConfig *config, XrModuleResolver **output) {
    if (!resources || !config || !output) return XR_MODULE_INVALID;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    if (config->lockfile && !xr_lockfile_uses_policy(config->lockfile,&policy)) return XR_MODULE_INVALID;
    if (config->catalog) {
        const XrXirCompileContext *context = xr_xir_compile_library_catalog_context(config->catalog);
        if (!context || context->resources != resources) return XR_MODULE_INVALID;
    }
    ModuleWork work = {resources,XR_MODULE_OK};
    XrModuleResolver *resolver = module_calloc(&work,1,sizeof(*resolver));
    if (!resolver) return work.status;
    resolver->resources = resources; resolver->config = *config;
    resolver->config.stdlib_path = module_dup(&work,config->stdlib_path);
    if (work.status == XR_MODULE_OK) module_io(&work,xr_hashmap_owned_new(&policy,&resolver->cache));
    if (work.status != XR_MODULE_OK) { xr_compile_module_resolver_free(resolver); return work.status; }
    *output = resolver; return XR_MODULE_OK;
}
XR_FUNC XrModuleStatus xr_compile_module_resolver_set_lockfile(XrModuleResolver *resolver, XrLockfile *lockfile) {
    if (!resolver || !resolver->resources || xr_hashmap_count(resolver->cache)) return XR_MODULE_INVALID;
    XrOsIoPolicy policy = xr_compile_io_policy(resolver->resources);
    if (lockfile && !xr_lockfile_uses_policy(lockfile,&policy)) return XR_MODULE_INVALID;
    resolver->config.lockfile = lockfile; return XR_MODULE_OK;
}
static bool relative_specifier(ModuleWork *work, const char *specifier) {
    return module_prefix(work,specifier,"./") || module_prefix(work,specifier,"../");
}
static void copy_authority(ModuleWork *work, const XrModuleIdentityAuthority *source, XrModuleIdentityAuthority *out) {
    out->kind = source->kind;
    out->namespace_id = module_dup(work,source->namespace_id);
    out->physical_root = module_dup(work,source->physical_root);
}
static bool copy_module_id(ModuleWork *work, const XrModuleId *source, XrModuleId *output) {
    XrModuleId copy = {0}; copy.kind = source->kind; copy.representation = source->representation; copy.resource = source->resource;
    copy.canonical = module_dup(work,source->canonical); copy.logical_path = module_dup(work,source->logical_path);
    copy.source_path = module_dup(work,source->source_path); copy_authority(work,&source->authority,&copy.authority);
    if (work->status != XR_MODULE_OK) { xr_compile_module_id_cleanup(&copy); return false; }
    *output = copy; return true;
}
static char *make_cache_key(ModuleWork *work, const char *specifier, const char *importer,
    const XrModuleIdentityAuthority *authority) {
    bool relative = relative_specifier(work,specifier);
    if (work->status != XR_MODULE_OK || (relative && !importer)) return NULL;
    char *identity = NULL, *logical = NULL;
    if (relative) module_status(work,xr_compile_module_identity_from_source(work->resources,authority,importer,&identity,&logical));
    char *key = module_format(work,"%s|%s",identity ? identity : "named-module-v1",specifier);
    xr_compile_resources_free(identity); xr_compile_resources_free(logical); return key;
}
static bool realpath_into(ModuleWork *work, const char *path, char **output) {
    XrOsIoPolicy policy = xr_compile_io_policy(work->resources);
    return work->status == XR_MODULE_OK && module_io(work,xr_realpath_owned(&policy,path,output));
}
static bool probe_file_import(ModuleWork *work, const char *base, const char *relative, char **output) {
    XrOsIoPolicy policy = xr_compile_io_policy(work->resources);
    const char *suffixes[] = {".xr","/index.xr"}; char path[XR_PATH_MAX];
    for (unsigned i = 0; i < 2 && module_work(work,1); ++i) {
        if (!module_format_buffer(work,path,sizeof(path),"%s/%s%s",base,relative,suffixes[i])) return false;
        XrOsIoStatus probe = xr_file_probe_owned(&policy,path,true);
        if (probe == XR_OS_IO_OK) return realpath_into(work,path,output);
        if (probe != XR_OS_IO_NOT_FOUND) return module_io(work,probe);
    }
    return module_status(work,XR_MODULE_NOT_FOUND);
}
static bool descriptor(ModuleWork *work, const char *name) {
    const XrStdlibSourceDescriptor *result = NULL;
    if (work->status == XR_MODULE_OK) module_io(work,
        xr_stdlib_source_descriptor_work(work,module_io_charge,name,&result));
    return result != NULL;
}
static bool stdlib_submodule_path(ModuleWork *work, const char *path, char *name, size_t capacity) {
    const char *slash = module_find_char(work,path,'/');
    if (!slash || slash == path || (size_t)(slash-path) >= capacity) return false;
    if (!module_copy(work,name,path,(size_t)(slash-path))) return false;
    name[slash-path] = 0;
    bool first = true;
    for (const char *p = path; module_work(work,1); ++p) {
        if (!*p) break;
        if (*p == '/') { if (first) return false; first = true; continue; }
        bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_';
        if (!letter && (first || *p < '0' || *p > '9')) return false;
        first = false;
    }
    return work->status == XR_MODULE_OK && !first && !module_equal(work,slash+1,name) && descriptor(work,name);
}
static void resolve_stdlib(ModuleWork *work, XrModuleResolver *resolver, const char *name, XrModuleId *out) {
    if (!descriptor(work,name)) { module_status(work,XR_MODULE_NOT_FOUND); return; }
    char logical[XR_PATH_MAX], path[XR_PATH_MAX];
    if (!module_format_buffer(work,logical,sizeof(logical),"%s/%s.xr",name,name)) return;
    out->kind = XR_MOD_STDLIB; out->authority.kind = XR_MODULE_IDENTITY_STDLIB;
    out->authority.namespace_id = module_dup(work,name);
    if (resolver->config.stdlib_path && module_format_buffer(work,path,sizeof(path),"%s/%s",resolver->config.stdlib_path,logical)) {
        XrOsIoPolicy policy = xr_compile_io_policy(work->resources);
        XrOsIoStatus probe = xr_file_probe_owned(&policy,path,true);
        if (probe == XR_OS_IO_OK) {
            realpath_into(work,path,&out->source_path);
            realpath_into(work,resolver->config.stdlib_path,(char **)&out->authority.physical_root);
        } else if (probe != XR_OS_IO_NOT_FOUND) module_io(work,probe);
    }
    if (work->status == XR_MODULE_OK) module_status(work,xr_compile_module_identity_from_logical(
        work->resources,&out->authority,logical,&out->canonical));
    out->logical_path = module_dup(work,logical);
}
static void resolve_stdlib_submodule(ModuleWork *work, XrModuleResolver *resolver, const char *specifier, XrModuleId *out) {
    char name[256], logical[XR_PATH_MAX], path[XR_PATH_MAX];
    if (!stdlib_submodule_path(work,specifier+4,name,sizeof(name))) { module_status(work,XR_MODULE_INVALID); return; }
    if (!resolver->config.stdlib_path) { module_status(work,XR_MODULE_NOT_FOUND); return; }
    if (!module_format_buffer(work,logical,sizeof(logical),"%s.xr",specifier+4) ||
        !module_format_buffer(work,path,sizeof(path),"%s/%s",resolver->config.stdlib_path,logical)) return;
    XrOsIoPolicy policy = xr_compile_io_policy(work->resources);
    if (!module_io(work,xr_file_probe_owned(&policy,path,true))) return;
    out->kind = XR_MOD_STDLIB; out->authority.kind = XR_MODULE_IDENTITY_STDLIB;
    out->authority.namespace_id = module_dup(work,name);
    realpath_into(work,path,&out->source_path);
    realpath_into(work,resolver->config.stdlib_path,(char **)&out->authority.physical_root);
    if (work->status == XR_MODULE_OK) module_status(work,xr_compile_module_identity_from_source(
        work->resources,&out->authority,out->source_path,&out->canonical,&out->logical_path));
    if (work->status == XR_MODULE_OK && !module_equal(work,out->logical_path,logical)) module_status(work,XR_MODULE_INVALID);
}
static void resolve_relative(ModuleWork *work, const char *specifier, const char *importer,
    const XrModuleIdentityAuthority *authority, XrModuleId *out) {
    if (!module_authority(work,authority)) return;
    if (authority->kind != XR_MODULE_IDENTITY_PROJECT && authority->kind != XR_MODULE_IDENTITY_SCRIPT &&
        authority->kind != XR_MODULE_IDENTITY_PACKAGE) { module_status(work,XR_MODULE_INVALID); return; }
    XrOsIoPolicy policy = xr_compile_io_policy(work->resources); char *base = NULL;
    if (importer) module_io(work,xr_path_dirname_owned(&policy,importer,&base));
    else { char cwd[XR_PATH_MAX]; if (module_io(work,xr_os_io_getcwd(&policy,cwd,sizeof(cwd)))) base = module_dup(work,cwd); }
    if (work->status == XR_MODULE_OK) probe_file_import(work,base,specifier,&out->source_path);
    xr_compile_resources_free(base);
    if (work->status != XR_MODULE_OK) return;
    out->kind = authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    module_status(work,xr_compile_module_identity_from_source(work->resources,authority,out->source_path,&out->canonical,&out->logical_path));
    copy_authority(work,authority,&out->authority);
}
static void resolve_package_source(ModuleWork *work, const XrModuleIdentityAuthority *authority, const char *path, XrModuleId *out) {
    out->kind = XR_MOD_PACKAGE;
    if (realpath_into(work,path,&out->source_path)) module_status(work,xr_compile_module_identity_from_source(
        work->resources,authority,out->source_path,&out->canonical,&out->logical_path));
    copy_authority(work,authority,&out->authority);
}
static void resolve_package(ModuleWork *work, XrModuleResolver *resolver, const char *specifier, XrModuleId *out) {
    const char *slash = module_find_char(work,specifier,'/');
    if (!slash || slash == specifier || (size_t)(slash-specifier) >= 64 || module_find_char(work,slash+1,'/')) {
        module_status(work,XR_MODULE_INVALID); return;
    }
    char owner[64], name[64]; size_t length = module_length(work,slash+1);
    if (!length || length >= sizeof(name)) { module_status(work,XR_MODULE_INVALID); return; }
    if (!module_copy(work,owner,specifier,(size_t)(slash-specifier)) || !module_copy(work,name,slash+1,length+1)) return;
    owner[slash-specifier] = 0;
    const XrLockedPackage *locked = NULL; XrOsIoPolicy policy = xr_compile_io_policy(work->resources);
    if (!resolver->config.lockfile) { module_status(work,XR_MODULE_INVALID); return; }
    XrOsIoStatus lookup = xr_lockfile_find_owned(&policy,resolver->config.lockfile,specifier,&locked);
    if (lookup == XR_OS_IO_NOT_FOUND) { module_status(work,XR_MODULE_INVALID); return; }
    if (!module_io(work,lookup)) return;
    bool version_valid = false;
    if (!locked || !locked->version || !locked->checksum) { module_status(work,XR_MODULE_INVALID); return; }
    if (!module_io(work,xr_semver_is_valid_owned(&policy,locked->version,&version_valid))) return;
    if (!version_valid || module_length(work,locked->checksum) != 71 || !module_prefix(work,locked->checksum,"sha256:")) {
        module_status(work,XR_MODULE_INVALID); return;
    }
    for (size_t i = 7; i < 71 && module_work(work,1); ++i)
        if (!isxdigit((unsigned char)locked->checksum[i])) { module_status(work,XR_MODULE_INVALID); return; }
    if (!module_work(work,1)) return;
    const char *home = getenv("HOME");
#ifdef XR_OS_WINDOWS
    if (!home && module_work(work,1)) home = getenv("USERPROFILE");
#endif
    if (!home) { module_status(work,XR_MODULE_INVALID); return; }
    char archive[XR_PATH_MAX], package_root[XR_PATH_MAX], coordinate[256];
    if (!module_format_buffer(work,archive,sizeof(archive),"%s/.xray/cache/%s-%s-%s.tar.gz",home,owner,name,locked->version)) return;
    if (!module_io(work,xr_file_probe_owned(&policy,archive,false))) return;
    bool checksum_valid = false;
    if (!module_io(work,xr_lockfile_verify_checksum_owned(&policy,archive,locked->checksum,&checksum_valid))) return;
    if (!checksum_valid) { module_status(work,XR_MODULE_INVALID); return; }
    if (!module_format_buffer(work,package_root,sizeof(package_root),"%s/.xray/packages/%s/%s/%s",home,owner,name,locked->version) ||
        !module_format_buffer(work,coordinate,sizeof(coordinate),"%s@%s",specifier,locked->version)) return;
    char *root = NULL; if (!realpath_into(work,package_root,&root)) return;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PACKAGE,coordinate,root};
    if (module_authority(work,&authority)) {
        const char *entries[] = {"src/main.xr","main.xr",NULL}; char path[XR_PATH_MAX]; bool found = false;
        for (unsigned i = 0; i < 3 && module_work(work,1); ++i) {
            if (!(i == 2 ? module_format_buffer(work,path,sizeof(path),"%s/%s.xr",root,name) :
                module_format_buffer(work,path,sizeof(path),"%s/%s",root,entries[i]))) break;
            XrOsIoStatus probe = xr_file_probe_owned(&policy,path,true);
            if (probe == XR_OS_IO_OK) { resolve_package_source(work,&authority,path,out); found = true; break; }
            if (probe != XR_OS_IO_NOT_FOUND) { module_io(work,probe); break; }
        }
        if (!found) module_status(work,XR_MODULE_NOT_FOUND);
    }
    xr_compile_resources_free(root);
}
#include "xmodule_checked_resource.inc.c"
XR_FUNC XrModuleStatus xr_compile_module_resolver_resolve(XrModuleResolver *resolver, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority, XrModuleId *output, char **error) {
    if (!resolver || !resolver->resources || !specifier || !output) return XR_MODULE_INVALID;
    ModuleWork work = {resolver->resources,XR_MODULE_OK}; XrModuleId result = {0}; bool matched = false;
    resolve_checked_resource(&work,resolver,specifier,importer,authority,&result,&matched);
    char *key = NULL; bool transferred = false;
    if (work.status == XR_MODULE_OK && !matched) {
        key = make_cache_key(&work,specifier,importer,authority);
        void *cached = NULL;
        if (key && work.status == XR_MODULE_OK) module_io(&work,xr_hashmap_owned_get(resolver->cache,key,&cached));
        if (cached) {
            const XrModuleId *found = cached;
            if (relative_specifier(&work,specifier) && (!authority || found->authority.kind != authority->kind ||
                !module_equal(&work,found->authority.namespace_id,authority->namespace_id) ||
                !module_equal(&work,found->authority.physical_root,authority->physical_root)))
                module_status(&work,XR_MODULE_INVALID);
            if (work.status == XR_MODULE_OK) copy_module_id(&work,found,&result);
        }
        else if (work.status == XR_MODULE_OK) {
            if (relative_specifier(&work,specifier)) resolve_relative(&work,specifier,importer,authority,&result);
            else if (module_prefix(&work,specifier,"std/")) resolve_stdlib_submodule(&work,resolver,specifier,&result);
            else if (module_find_char(&work,specifier,'/')) resolve_package(&work,resolver,specifier,&result);
            else if (work.status == XR_MODULE_OK) resolve_stdlib(&work,resolver,specifier,&result);
            if (key && work.status == XR_MODULE_OK) {
                XrModuleId *copy = module_calloc(&work,1,sizeof(*copy));
                if (copy && copy_module_id(&work,&result,copy) &&
                    module_io(&work,xr_hashmap_owned_set(resolver->cache,key,copy))) transferred = true;
                if (!transferred) { xr_compile_module_id_cleanup(copy); xr_compile_resources_free(copy); }
            }
        }
    }
    if (!transferred) xr_compile_resources_free(key);
    if (work.status == XR_MODULE_OK) *output = result;
    else {
        xr_compile_module_id_cleanup(&result);
        if (work.status != XR_MODULE_BUDGET && work.status != XR_MODULE_OUT_OF_MEMORY)
            module_error(resolver->resources,error,work.status == XR_MODULE_NOT_FOUND ? "module not found" : "module resolution failed");
    }
    return work.status;
}
