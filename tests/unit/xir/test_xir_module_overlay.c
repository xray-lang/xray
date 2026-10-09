/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "base/xmalloc.h"
#include "module/xmodule_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
static XrCompileResourceLimits overlay_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
}
static void overlay_zero(void) { CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); }
static XrModuleStatus overlay_make(XrCompileResourceLimits limits, XrModuleOverlay **output,
    XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(&limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status == XR_COMPILE_RESOURCE_BUDGET ? XR_MODULE_BUDGET : XR_MODULE_OUT_OF_MEMORY;
    char names[8][16], paths[8][64], text[] = "overlay payload";
    char root[] = "/overlay", coordinate[] = "workspace";
    XrModuleOverlayInput inputs[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(snprintf(names[i], sizeof(names[i]), "file%u.xr", i) > 0);
        CHECK(snprintf(paths[i], sizeof(paths[i]), "/overlay/%s", names[i]) > 0);
        inputs[i] = (XrModuleOverlayInput){{XR_MODULE_IDENTITY_PROJECT, coordinate, root}, names[i], paths[i], text, sizeof(text)-1};
    }
    XrModuleStatus result = xr_compile_module_overlay_new(resources, inputs, 8, output);
    memset(names, '?', sizeof(names)); memset(paths, '?', sizeof(paths)); memset(text, '?', sizeof(text));
    memset(root, '?', sizeof(root)); memset(coordinate, '?', sizeof(coordinate)); memset(inputs, 0, sizeof(inputs));
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);
    return result;
}
static void overlay_facts(XrModuleOverlay *overlay) {
    size_t count = 0; const XrModuleOverlayEntry *entries = xr_compile_module_overlay_entries(overlay, &count);
    CHECK(count == 8 && entries && xr_compile_module_overlay_resources(overlay));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PROJECT, "workspace", "/overlay"};
    for (unsigned i = 0; i < 8; ++i) {
        char key[128], path[64], logical[16];
        CHECK(snprintf(logical, sizeof(logical), "file%u.xr", i) > 0);
        CHECK(snprintf(key, sizeof(key), "module-id-v1:kind=7:project:namespace=9:workspace:path=8:%s", logical) > 0);
        CHECK(snprintf(path, sizeof(path), "/overlay/%s", logical) > 0);
        const XrModuleOverlayEntry *found = NULL;
        CHECK(xr_compile_module_overlay_lookup(overlay, key, &authority, &found) == XR_MODULE_OK);
        CHECK(found == &entries[i] && !strcmp(found->canonical, key));
        CHECK(!strcmp(found->source.logical_path, logical) && !strcmp(found->source.source_path, path));
        CHECK(found->source.length == 15 && !memcmp(found->source.text, "overlay payload", 15) && !found->source.text[15]);
    }
    const XrModuleOverlayEntry *canary = entries;
    CHECK(xr_compile_module_overlay_lookup(overlay, "missing", &authority, &canary) == XR_MODULE_OK && !canary);
    authority.physical_root = "/another"; canary = entries+1;
    CHECK(xr_compile_module_overlay_lookup(overlay, entries[0].canonical, &authority, &canary) == XR_MODULE_INVALID);
    CHECK(canary == entries+1);
    authority.physical_root = "/overlay"; authority.namespace_id = "another";
    CHECK(xr_compile_module_overlay_lookup(overlay, entries[0].canonical, &authority, &canary) == XR_MODULE_INVALID);
    CHECK(canary == entries+1);
}
static void overlay_failures(void) {
    size_t sites = 0; XrCompileResourceStats required = {0};
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = probe ? probe-1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrModuleOverlay *overlay = NULL; XrCompileResourceStats stats = {0};
        XrModuleStatus status = overlay_make(overlay_limits(), &overlay, &stats);
        if (!probe) {
            CHECK(status == XR_MODULE_OK && overlay); sites = source_fixture_compile_attempts; CHECK(sites);
            required = stats; overlay_facts(overlay);
        } else {
            CHECK(status == XR_MODULE_OUT_OF_MEMORY && !overlay && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts > source_fixture_compile_fail_at);
        }
        xr_compile_module_overlay_free(overlay); overlay_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected = false;
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes, required.peak_bytes, required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound + delta);
        XrModuleOverlay *overlay = NULL; XrCompileResourceStats stats = {0};
        CHECK(overlay_make(limits, &overlay, &stats) == (delta < 0 ? XR_MODULE_BUDGET : XR_MODULE_OK));
        CHECK((overlay != NULL) == (delta >= 0));
        /* Exact construction work leaves no allowance for a separate lookup. */
        xr_compile_module_overlay_free(overlay); overlay_zero();
    }
    printf("Overlay owner fresh allocation sites=%zu; exact/minus1/plus1, physical=0/0\n", sites);
}
static void overlay_rejections(void) {
    for (unsigned mode = 0; mode < 14; ++mode) {
        XrCompileResources *resources = NULL; XrCompileResourceLimits limits = overlay_limits();
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        const char embedded[] = {'a', 0, 'b'};
        XrModuleOverlayInput inputs[2] = {
            {{XR_MODULE_IDENTITY_SCRIPT, NULL, "/overlay"}, "root.xr", "/overlay/root.xr", "", 0},
            {{XR_MODULE_IDENTITY_SCRIPT, NULL, "/overlay"}, "lib.xr", "/overlay/lib.xr", "abc", 3}};
        if (mode == 0) inputs[1] = inputs[0];
        if (mode == 1) inputs[1].logical_path = "other.xr";
        if (mode == 2) inputs[1].source_path = "/elsewhere/lib.xr";
        if (mode == 3) inputs[1].text = NULL;
        if (mode == 4) inputs[1].text = embedded;
        if (mode == 5) inputs[1].length = SIZE_MAX;
        if (mode == 6) inputs[1].source_path = "/overlay/./lib.xr";
        if (mode == 7) inputs[1].authority.physical_root = "relative";
        if (mode == 8) inputs[1].source_path = "C:lib.xr";
        if (mode == 9) inputs[1].source_path = "\\\\?\\C:\\overlay\\lib.xr";
        if (mode == 10) inputs[1].source_path = "/overlay/LIB.xr";
        if (mode == 11) {
            inputs[1].authority.physical_root = "/elsewhere";
            inputs[1].logical_path = "root.xr"; inputs[1].source_path = "/elsewhere/root.xr";
        }
        if (mode == 12) inputs[1].source_path = "/overlay2/lib.xr";
        if(mode==13) {
            inputs[1].authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_PROJECT,"other", "/overlay"};
            inputs[1].logical_path="root.xr";inputs[1].source_path="/overlay/root.xr";
        }
        XrModuleOverlay *overlay = NULL;
        CHECK(xr_compile_module_overlay_new(resources, inputs, 2, &overlay) ==
            (mode==11?XR_MODULE_OK:mode==5?XR_MODULE_BUDGET:XR_MODULE_INVALID));
        if(mode==11) {
            /* Portable script identity is intentionally independent of physical
             * root: two distinct editor documents retain distinct text. */
            size_t count=0;const XrModuleOverlayEntry *entries=xr_compile_module_overlay_entries(overlay,&count);CHECK(count==2);
            const XrModuleOverlayEntry *found=NULL;
            CHECK(xr_compile_module_overlay_lookup(overlay,entries[0].canonical,&inputs[0].authority,&found)==XR_MODULE_OK&&found==entries);
            found=NULL;
            CHECK(xr_compile_module_overlay_lookup(overlay,entries[1].canonical,&inputs[1].authority,&found)==XR_MODULE_OK&&found==entries+1);
            CHECK(found->source.length==3&&!memcmp(found->source.text,"abc",3));
            xr_compile_module_overlay_free(overlay);
        } else CHECK(!overlay);
        xr_compile_resources_release(resources); overlay_zero();
    }
}
static void overlay_paths(void) {
    const XrModuleOverlayInput inputs[] = {
        {{XR_MODULE_IDENTITY_SCRIPT, NULL, "C:\\work"}, "root.xr", "C:/work/root.xr", "x", 1},
        {{XR_MODULE_IDENTITY_SCRIPT, NULL, "\\\\host\\share\\work"}, "root.xr", "//host/share/work/root.xr", "x", 1},
        {{XR_MODULE_IDENTITY_MEMORY, "overlay", NULL}, NULL, NULL, "", 0},
#ifdef XR_OS_WINDOWS
        {{XR_MODULE_IDENTITY_SCRIPT, NULL, "C:\\WORK"}, "root.xr", "c:/work/root.xr", "x", 1},
#endif
    };
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        XrCompileResources *resources = NULL; XrCompileResourceLimits limits = overlay_limits();
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrModuleOverlay *overlay = NULL;
        CHECK(xr_compile_module_overlay_new(resources, &inputs[i], 1, &overlay) == XR_MODULE_OK);
        const XrModuleOverlayEntry *entries = xr_compile_module_overlay_entries(overlay, NULL), *found = NULL;
        const char *key = i == 2 ? "memory-module-v1:id=7:overlay" : "module-id-v1:kind=6:script:namespace=0::path=7:root.xr";
        CHECK(entries && !strcmp(entries->canonical, key));
        CHECK(xr_compile_module_overlay_lookup(overlay, key, &inputs[i].authority, &found) == XR_MODULE_OK && found == entries);
        XrModuleOverlay *saved = overlay;
        CHECK(xr_compile_module_overlay_new(resources, inputs, 1, &overlay) == XR_MODULE_INVALID && overlay == saved);
        XrCompileResourceStats stats = {0}; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_work(resources, limits.work-stats.work) == XR_COMPILE_RESOURCE_OK);
        found = entries;
        CHECK(xr_compile_module_overlay_lookup(overlay, key, &inputs[i].authority, &found) == XR_MODULE_BUDGET && found == entries);
        xr_compile_resources_release(resources); xr_compile_module_overlay_free(overlay); overlay_zero();
    }
}
int main(void) {
    XrCompileResources *resources = NULL; XrCompileResourceLimits limits = overlay_limits();
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrModuleOverlay *empty = NULL;
    CHECK(xr_compile_module_overlay_new(resources, NULL, 1, &empty) == XR_MODULE_INVALID && !empty);
    CHECK(xr_compile_module_overlay_new(resources, NULL, 0, &empty) == XR_MODULE_OK && empty);
    size_t count = 1; CHECK(!xr_compile_module_overlay_entries(empty, &count) && !count);
    xr_compile_module_overlay_free(empty); xr_compile_resources_release(resources); overlay_zero();
    overlay_failures(); overlay_rejections(); overlay_paths();
    puts("Immutable overlay owner: copied coordinates/text, canonical lookup, rejected aliases and physical cleanup passed");
    return 0;
}
