#include "analysis/xglobal_producer.h"
#include "base/xmalloc.h"
#include "module/xmodule_graph.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <stdlib.h>
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif
#else
#include <unistd.h>
#endif

static int fixture_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static char *fixture_realpath(const char *path, char *resolved) {
#ifdef _WIN32
    return _fullpath(resolved, path, PATH_MAX);
#else
    return realpath(path, resolved);
#endif
}

static bool mkdir_one(const char *path) {
    if (fixture_mkdir(path) == 0)
        return true;
    return errno == EEXIST;
}

static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len;
    if (!path)
        return false;
    len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return false;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (!mkdir_one(buf))
                return false;
            *p = path[p - buf];
        }
    }
    return mkdir_one(buf);
}

static bool write_file_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    if (fputs(text, f) < 0) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

static bool add_fixture_link_dependency(XgGlobalEvidence *package, XgModuleId module_id) {
    XgLinkDependencySummary dep;
    memset(&dep, 0, sizeof(dep));
    dep.link_id = 1;
    dep.module_id = module_id;
    dep.kind = XG_LINK_DEP_STDLIB_SYMBOL;
    dep.name_id = xg_name_id("math.abs");
    snprintf(dep.name, sizeof(dep.name), "%s", "math.abs");
    return xg_global_evidence_add_link_dependency(package, &dep) != NULL;
}

static bool write_global_payload_to_cache(const char *cache_dir, const char *payload,
                                          char *out_path, size_t out_path_size) {
    XgEvidenceCachePayloadInfo info;
    char phase_dir[PATH_MAX];
    int n;
    if (!cache_dir || !payload || !out_path || out_path_size == 0 ||
        !xg_evidence_cache_payload_parse(payload, &info))
        return false;
    n = snprintf(phase_dir, sizeof(phase_dir), "%s/evidence/%s", cache_dir,
                 xg_evidence_cache_phase_name(XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE));
    if (n < 0 || (size_t) n >= sizeof(phase_dir) || !mkdir_p(phase_dir))
        return false;
    n = snprintf(out_path, out_path_size, "%s/%016" PRIx64 ".xgpayload", phase_dir, info.key_hash);
    if (n < 0 || (size_t) n >= out_path_size)
        return false;
    return write_file_text(out_path, payload);
}

int main(int argc, char **argv) {
    const char *cache_dir;
    const char *canonical;
    const char *source_path;
    char real_source[PATH_MAX];
    char payload_path[PATH_MAX];
    XrModuleSpec spec = {0};
    XgBuildKey key;
    XgModuleSummary module;
    XgGlobalEvidence package;
    char *payload = NULL;
    int rc = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s CACHE_DIR PACKAGE_CANONICAL SOURCE_PATH\n", argv[0]);
        return 2;
    }

    cache_dir = argv[1];
    canonical = argv[2];
    source_path = argv[3];
    if (!fixture_realpath(source_path, real_source)) {
        fprintf(stderr, "package_payload_fixture: cannot resolve source path: %s\n", source_path);
        return 1;
    }

    spec.canonical = (char *) canonical;
    spec.source_path = real_source;
    spec.kind = XR_MOD_PACKAGE;

    if (!xg_standalone_build_key_from_module_spec(&key, &spec, XG_BUILD_NATIVE_RELEASE, 0) ||
        !xg_module_summary_from_module_spec(&module, 1, &spec)) {
        fprintf(stderr, "package_payload_fixture: failed to derive package evidence identity\n");
        return 1;
    }

    memset(&package, 0, sizeof(package));
    xg_global_evidence_init(&package, key);
    if (!xg_global_evidence_add_module(&package, &module) ||
        !add_fixture_link_dependency(&package, module.module_id)) {
        fprintf(stderr, "package_payload_fixture: failed to build package evidence rows\n");
        goto done;
    }

    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    if (!payload ||
        !write_global_payload_to_cache(cache_dir, payload, payload_path, sizeof(payload_path))) {
        fprintf(stderr, "package_payload_fixture: failed to write package payload\n");
        goto done;
    }

    printf("%s\n", payload_path);
    rc = 0;

done:
    xr_free(payload);
    xg_global_evidence_free(&package);
    return rc;
}
