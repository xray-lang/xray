/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproject.c - Project configuration implementation
 */

#include "xproject.h"
#include "xsemver.h"
#include "xmanifest_owner.inc.h"
#include "xmodule_identity_internal.h"
#include "../os/os_fs.h"
#include <stddef.h>

struct XrProjectAuthority { XrModuleIdentityAuthority view; char *namespace_id; char *root; };
static void free_dependency(XrDependency *dep);
static void free_target_config(XrTargetConfig *cfg);

static char *project_string(ManifestContext *ctx, XrTomlValue *table, const char *key) {
    const char *text=manifest_string(ctx,table,key);
    return text ? manifest_duplicate(ctx,text) : NULL;
}
static bool project_string_array(ManifestContext *ctx, XrTomlValue *table, const char *key,
    char ***output, int *count) {
    XrTomlValue *array=manifest_get_type(ctx,table,key,XR_TOML_ARRAY);
    if (!array) return ctx->status==XR_MANIFEST_OK;
    if (!manifest_work(ctx,1)) return false;
    int length=array->as.array.count;
    char **items=manifest_calloc(ctx,(size_t)length,sizeof(*items));
    if (!items) return false;
    for (int i=0;i<length;++i) {
        if (!manifest_work(ctx,1)) break;
        XrTomlValue *item=array->as.array.items[i];
        if (!item || item->type!=XR_TOML_STRING) { manifest_error(ctx,"target string array contains a non-string"); break; }
        items[i]=manifest_duplicate(ctx,item->as.string);
        if (!items[i]) break;
    }
    if (ctx->status!=XR_MANIFEST_OK) {
        for (int i=0;i<length;++i) manifest_free(items[i]);
        manifest_free(items);return false;
    }
    *output=items;*count=length;return true;
}
static XrTargetConfig *project_target(ManifestContext *ctx, const char *name, XrTomlValue *table) {
    if (!table || table->type!=XR_TOML_TABLE) { manifest_error(ctx,"target entry must be a table");return NULL; }
    XrTargetConfig *target=manifest_calloc(ctx,1,sizeof(*target));
    if (!target) return NULL;
    target->name=manifest_duplicate(ctx,name);
#define PROJECT_STRING(field) target->field=project_string(ctx,table,#field)
    PROJECT_STRING(profile);PROJECT_STRING(toolchain);PROJECT_STRING(cc);PROJECT_STRING(zig);
    PROJECT_STRING(sysroot);PROJECT_STRING(linker_script);PROJECT_STRING(objcopy);
    PROJECT_STRING(objcopy_output);PROJECT_STRING(runtime_provider);
#undef PROJECT_STRING
#define PROJECT_ARRAY(field) project_string_array(ctx,table,#field,&target->field,&target->n_##field)
    PROJECT_ARRAY(runtime_capabilities);PROJECT_ARRAY(runtime_hooks);PROJECT_ARRAY(cc_flags);
    PROJECT_ARRAY(ld_flags);PROJECT_ARRAY(objcopy_flags);
#undef PROJECT_ARRAY
    if (ctx->status!=XR_MANIFEST_OK) { free_target_config(target);return NULL; }
    return target;
}

XR_FUNC XrManifestStatus xr_project_load_owned(const XrOsIoPolicy *policy,
    const char *absolute_root, const XrTomlParseLimits *limits, XrProject **output,
    XrManifestDiagnostic *diagnostic) {
    if (!io_policy_valid(policy)||!absolute_root||!limits||!output||*output) return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*policy,XR_MANIFEST_OK,diagnostic};
    if (!manifest_absolute(&ctx,absolute_root))
        return ctx.status==XR_MANIFEST_OK ? XR_MANIFEST_BAD_ARGUMENT : ctx.status;
    XrFileBytes bytes={0};XrTomlValue *document=NULL;XrProject *project=NULL;
    manifest_read_under_root(&ctx,absolute_root,"xray.toml",limits->input_bytes,&bytes);
    if (ctx.status==XR_MANIFEST_OK) manifest_toml_status(&ctx,xtoml_parse_owned(policy,bytes.data,bytes.size,limits,&document));
    manifest_free(bytes.data);
    if (ctx.status!=XR_MANIFEST_OK) goto done;
    project=manifest_calloc(&ctx,1,sizeof(*project));
    if (!project) goto done;
    project->root=manifest_realpath(&ctx,absolute_root);
    if (ctx.status==XR_MANIFEST_OK) manifest_io(&ctx,xr_hashmap_owned_new(policy,&project->dependencies));
    if (ctx.status==XR_MANIFEST_OK) manifest_io(&ctx,xr_hashmap_owned_new(policy,&project->targets));
    XrTomlValue *section=manifest_get_type(&ctx,document,"project",XR_TOML_TABLE);
    if (!section && ctx.status==XR_MANIFEST_OK) {
        section=manifest_get_type(&ctx,document,"package",XR_TOML_TABLE);
        project->is_package=section!=NULL;
    }
    if (section) {
        project->name=project_string(&ctx,section,"name");
        project->main=project_string(&ctx,section,"main");
        if (project->is_package) {
            project->version=project_string(&ctx,section,"version");
            project->description=project_string(&ctx,section,"description");
            project->license=project_string(&ctx,section,"license");
        }
    }
    XrTomlValue *dependencies=manifest_get_type(&ctx,document,"dependencies",XR_TOML_TABLE);
    if (dependencies) for (int i=0;i<dependencies->as.table.count;++i) {
        if (!manifest_work(&ctx,1)) break;
        XrTomlMember *member=&dependencies->as.table.members[i];
        XrDependency *dependency=manifest_calloc(&ctx,1,sizeof(*dependency));
        if (!dependency) break;
        dependency->name=manifest_duplicate(&ctx,member->key);
        if (member->value->type==XR_TOML_STRING)
            dependency->version=manifest_duplicate(&ctx,member->value->as.string);
        else if (member->value->type==XR_TOML_TABLE) {
            dependency->version=project_string(&ctx,member->value,"version");
            dependency->path=project_string(&ctx,member->value,"path");
            dependency->is_local=dependency->path!=NULL;
        } else manifest_error(&ctx,"dependency must be a version string or a table");
        if (ctx.status==XR_MANIFEST_OK)
            manifest_io(&ctx,xr_hashmap_owned_set(project->dependencies,dependency->name,dependency));
        if (ctx.status!=XR_MANIFEST_OK) { free_dependency(dependency);break; }
    }
    XrTomlValue *targets=manifest_get_type(&ctx,document,"target",XR_TOML_TABLE);
    if (targets) for (int i=0;i<targets->as.table.count;++i) {
        if (!manifest_work(&ctx,1)) break;
        XrTomlMember *member=&targets->as.table.members[i];
        XrTargetConfig *target=project_target(&ctx,member->key,member->value);
        if (!target) break;
        manifest_io(&ctx,xr_hashmap_owned_set(project->targets,target->name,target));
        if (ctx.status!=XR_MANIFEST_OK) { free_target_config(target);break; }
    }
    if (ctx.status==XR_MANIFEST_OK) {
        XrManifestStatus status=xr_native_package_plan_parse_owned(policy,document,project->root,&project->native_plan,diagnostic);
        if (status!=XR_MANIFEST_NOT_FOUND) manifest_status(&ctx,status);
    }
    if (ctx.status==XR_MANIFEST_OK && manifest_work(&ctx,1)) {
        project->initialized=true;*output=project;project=NULL;
    }
done:
    xtoml_owned_free(document);xr_project_free_owned(project);return ctx.status;
}

static void free_dependency(XrDependency *dep) {
    if (!dep)
        return;
    manifest_free(dep->name);
    manifest_free(dep->version);
    manifest_free(dep->path);
    manifest_free(dep);
}

static void free_string_list(char **items, int count) {
    if (!items)
        return;
    for (int i = 0; i < count; i++)
        manifest_free(items[i]);
    manifest_free(items);
}

static void free_target_config(XrTargetConfig *cfg) {
    if (!cfg)
        return;
    manifest_free(cfg->name);
    manifest_free(cfg->profile);
    manifest_free(cfg->toolchain);
    manifest_free(cfg->cc);
    manifest_free(cfg->zig);
    manifest_free(cfg->sysroot);
    manifest_free(cfg->linker_script);
    manifest_free(cfg->objcopy);
    manifest_free(cfg->objcopy_output);
    manifest_free(cfg->runtime_provider);
    free_string_list(cfg->runtime_capabilities, cfg->n_runtime_capabilities);
    free_string_list(cfg->runtime_hooks, cfg->n_runtime_hooks);
    free_string_list(cfg->cc_flags, cfg->n_cc_flags);
    free_string_list(cfg->ld_flags, cfg->n_ld_flags);
    free_string_list(cfg->objcopy_flags, cfg->n_objcopy_flags);
    manifest_free(cfg);
}


static void dispose_dependency(const char *key, void *value, void *context) {
    (void)key;(void)context;free_dependency(value);
}
static void dispose_target(const char *key, void *value, void *context) {
    (void)key;(void)context;free_target_config(value);
}
XR_FUNC void xr_project_free_owned(XrProject *project) {
    if (!project) return;
    manifest_free(project->root);manifest_free(project->name);manifest_free(project->main);
    manifest_free(project->version);manifest_free(project->description);manifest_free(project->license);
    xr_hashmap_owned_dispose(project->dependencies,dispose_dependency,NULL);
    xr_hashmap_owned_dispose(project->targets,dispose_target,NULL);
    xr_native_package_plan_free_owned(project->native_plan);manifest_free(project);
}
XR_FUNC bool xr_project_uses_policy(const XrProject *project, const XrOsIoPolicy *policy) {
    return manifest_uses_policy(project,policy);
}
static bool project_identity_work(void *opaque, uint64_t units) { return manifest_work(opaque,units); }
XR_FUNC XrManifestStatus xr_project_authority_build_owned(const XrProject *project,
    XrProjectAuthority **output, XrManifestDiagnostic *diagnostic) {
    if (!project||!output||*output) return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(project),XR_MANIFEST_OK,diagnostic};
    if (!project->name||!project->name[0]) { manifest_error(&ctx,"manifest declares no project or package name");return ctx.status; }
    bool valid=true;
    if (project->is_package) {
        if (!project->version) valid=false;
        else manifest_io(&ctx,xr_semver_is_valid_owned(&ctx.policy,project->version,&valid));
        if (ctx.status==XR_MANIFEST_OK&&!valid) manifest_error(&ctx,"package version must be an exact semantic version");
    }
    XrProjectAuthority *authority=manifest_calloc(&ctx,1,sizeof(*authority));
    if (!authority) return ctx.status;
    authority->root=manifest_duplicate(&ctx,project->root);
    if (project->is_package) {
        size_t a=manifest_length(&ctx,project->name),b=manifest_length(&ctx,project->version);
        if (a>SIZE_MAX-2||b>SIZE_MAX-a-2) manifest_status(&ctx,XR_MANIFEST_BUDGET);
        if (ctx.status==XR_MANIFEST_OK)authority->namespace_id=manifest_alloc(&ctx,a+b+2);
        if (authority->namespace_id&&manifest_work(&ctx,a+b+2)) {
            memcpy(authority->namespace_id,project->name,a);authority->namespace_id[a]='@';
            memcpy(authority->namespace_id+a+1,project->version,b+1);
        }
    } else authority->namespace_id=manifest_duplicate(&ctx,project->name);
    authority->view.kind=project->is_package?XR_MODULE_IDENTITY_PACKAGE:XR_MODULE_IDENTITY_PROJECT;
    authority->view.namespace_id=authority->namespace_id;authority->view.physical_root=authority->root;
    XrModuleIdentityWork work={&ctx,project_identity_work,XR_MODULE_OK};
    if (ctx.status==XR_MANIFEST_OK&&!xr_module_identity_authority_walk(&work,&authority->view))
        manifest_error(&ctx,"manifest name is not an exact project or owner/package coordinate");
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) { *output=authority;authority=NULL; }
    xr_project_authority_free_owned(authority);return ctx.status;
}
XR_FUNC const XrModuleIdentityAuthority *xr_project_authority_view(const XrProjectAuthority *authority) {
    return authority?&authority->view:NULL;
}
XR_FUNC void xr_project_authority_free_owned(XrProjectAuthority *authority) {
    if (!authority)return;
    manifest_free(authority->namespace_id);manifest_free(authority->root);manifest_free(authority);
}
XR_FUNC XrManifestStatus xr_resolve_local_dependency_owned(const XrProject *project,
    const char *name, char **output) {
    if (!project||!name||!output||*output) return XR_MANIFEST_BAD_ARGUMENT;
    ManifestContext ctx={*manifest_policy(project),XR_MANIFEST_OK,NULL};void *found=NULL;
    manifest_io(&ctx,xr_hashmap_owned_get(project->dependencies,name,&found));
    if (ctx.status!=XR_MANIFEST_OK)return ctx.status;
    XrDependency *dependency=found;char *path=NULL;
    if (dependency&&dependency->is_local&&dependency->path) {
        bool absolute=manifest_absolute(&ctx,dependency->path);
        path=absolute?manifest_duplicate(&ctx,dependency->path):manifest_join(&ctx,project->root,dependency->path);
    }
    if (ctx.status==XR_MANIFEST_OK&&manifest_work(&ctx,1)) { *output=path;path=NULL; }
    manifest_free(path);return ctx.status;
}
XR_FUNC void xr_project_path_free_owned(char *path) { manifest_free(path); }
XR_FUNC XrManifestStatus xr_project_find_target_config_owned(const XrProject *project,
    const char *name, const XrTargetConfig **output) {
    if (!project||!name||!output)return XR_MANIFEST_BAD_ARGUMENT;
    void *found=NULL;XrOsIoStatus status=xr_hashmap_owned_get(project->targets,name,&found);
    if (status==XR_OS_IO_OK) {
        const XrOsIoPolicy *policy=manifest_policy(project);status=policy->work(policy->context,1);
        if (status==XR_OS_IO_OK)*output=found;
    }
    return manifest_io_status(status);
}
