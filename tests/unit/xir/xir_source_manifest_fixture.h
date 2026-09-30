/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_manifest_fixture.h - Private source and declaration files
 */
#ifndef XIR_SOURCE_MANIFEST_FIXTURE_H
#define XIR_SOURCE_MANIFEST_FIXTURE_H
#include "../test_win_compat.h"
typedef struct SourceTestDeclaration {
    const char *name, *owner, *parameter;
    bool no_suspend;
} SourceTestDeclaration;
typedef struct SourceTestFiles {
    char directory[64], root[4096], entry[8192];
} SourceTestFiles;
static void source_fixture_path(const XrXirSourceRequest *request, const char *name, char path[8192]) {
    int written = snprintf(path, 8192, "%s/%s", request->authority->physical_root, name);
    CHECK(written > 0 && written < 8192);
}
static void source_manifest_raw(const XrXirSourceRequest *request, const char *text) {
    char path[8192]; source_fixture_path(request, "xray.toml", path);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs(text, file) >= 0 && fclose(file) == 0);
}
static void source_manifest_write(const XrXirSourceRequest *request, const char *module,
    const SourceTestDeclaration *records, uint32_t count) {
    char path[8192]; source_fixture_path(request, "xray.toml", path);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs("[declarations]\nversion=1\n", file) >= 0);
    for (uint32_t i = 0; i < count; ++i) {
        const SourceTestDeclaration *item = &records[i];
        CHECK(fprintf(file, "[[declarations.function]]\nmodule=\"%s\"\nname=\"%s\"\n", module, item->name) > 0);
        if (item->owner) CHECK(fprintf(file, "owner=\"%s\"\n", item->owner) > 0);
        if (item->parameter) CHECK(fprintf(file, "no_suspend_parameters=[\"%s\"]\n", item->parameter) > 0);
        if (item->no_suspend) CHECK(fputs("no_suspend=true\n", file) >= 0);
    }
    CHECK(fclose(file) == 0);
}
static void source_files_open(SourceTestFiles *files) {
    memcpy(files->directory, "xir-effect-files-XXXXXX", sizeof("xir-effect-files-XXXXXX"));
    CHECK(xr_test_mkdtemp(files->directory));
    CHECK(xr_test_realpath_buf(files->directory, files->root, sizeof(files->root)));
    const char *names[] = {"root.xr", "methods.xr", "default_selector.xr"};
    for (uint32_t i = 0; i < 3; ++i) {
        char from[8192], to[8192];
        CHECK(snprintf(from, sizeof(from), "%s/%s", XR_EFFECT_FIXTURES, names[i]) > 0);
        CHECK(snprintf(to, sizeof(to), "%s/%s", files->root, names[i]) > 0);
        FILE *source = fopen(from, "rb"), *target = fopen(to, "wb"); CHECK(source && target);
        char bytes[4096]; size_t count;
        while ((count = fread(bytes, 1, sizeof(bytes), source)) != 0)
            CHECK(fwrite(bytes, 1, count, target) == count);
        CHECK(!ferror(source) && fclose(source) == 0 && fclose(target) == 0);
    }
    CHECK(snprintf(files->entry, sizeof(files->entry), "%s/root.xr", files->root) > 0);
}
static void source_files_close(const SourceTestFiles *files) {
    const char *names[] = {"root.xr", "methods.xr", "default_selector.xr", "xray.toml"};
    for (uint32_t i = 0; i < 4; ++i) {
        char path[8192]; CHECK(snprintf(path, sizeof(path), "%s/%s", files->root, names[i]) > 0);
        CHECK(xr_test_unlink(path) == 0);
    }
    CHECK(xr_test_rmdir(files->directory) == 0);
}
#endif // XIR_SOURCE_MANIFEST_FIXTURE_H
