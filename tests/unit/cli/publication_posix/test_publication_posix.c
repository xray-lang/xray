/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_publication_posix.c - Directory-descriptor output replacement
 *
 * KEY CONCEPT:
 *   Every outcome leaves either the old entry or the complete new bytes, never
 *   a staged temporary file, and returns all memory to its owner.
 */
#include "app/toolchain/xtc_xir_publication.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line=%d %s errno=%d\n", __LINE__, #c, errno); exit(90); } } while (0)

static XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
static const char payload[] = "complete\0bytes";
static char root[4096];

static void path_in(char *out, size_t size, const char *leaf) {
    CHECK((size_t)snprintf(out, size, "%s/%s", root, leaf) < size);
}
static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file); CHECK(fwrite(text, 1, strlen(text), file) == strlen(text)); CHECK(!fclose(file));
}
static void file_is(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "rb"); CHECK(file);
    char actual[64]; size_t count = fread(actual, 1, sizeof(actual), file);
    CHECK(!ferror(file) && !fclose(file)); CHECK(count == length && !memcmp(actual, bytes, length));
}
/* Only the named entries may exist: no staged ".xray-*.tmp" survives. */
static void directory_holds(unsigned expected) {
    DIR *directory = opendir(root); CHECK(directory);
    unsigned count = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(strncmp(entry->d_name, ".xray-", 6)); ++count;
    }
    CHECK(!closedir(directory)); CHECK(count == expected);
}
static XtcXirPublicationStatus replace(const char *path, bool commit, uint64_t max_bytes) {
    XrCompileResources *resources = NULL; XtcXirPublication *owner = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats empty = {0}, closed = {0};
    CHECK(xr_compile_resources_stats(resources, &empty) == XR_COMPILE_RESOURCE_OK);
    XtcXirPublicationRequest request = {path, {4096, 8, max_bytes}, false};
    XtcXirPublicationStatus status = xtc_xir_publication_new(resources, &request, &owner);
    if (status == XTC_XIR_PUBLICATION_OK) status = xtc_xir_publication_write(owner, payload, sizeof(payload) - 1);
    if (status == XTC_XIR_PUBLICATION_OK && commit) status = xtc_xir_publication_commit(owner);
    if (status != XTC_XIR_PUBLICATION_OK && owner) {
        CHECK(xtc_xir_publication_write(owner, payload, sizeof(payload) - 1) == status);
        CHECK(xtc_xir_publication_commit(owner) == status);
        CHECK(!xtc_xir_publication_diagnostic(owner)->published);
    }
    CHECK(xtc_xir_publication_close(&owner) == XTC_XIR_PUBLICATION_OK && !owner);
    CHECK(xr_compile_resources_stats(resources, &closed) == XR_COMPILE_RESOURCE_OK);
    CHECK(closed.held_bytes == empty.held_bytes);
    xr_compile_resources_release(resources);
    return status;
}
static void replacement(void) {
    char path[4200]; path_in(path, sizeof(path), "out.c");
    CHECK(replace(path, true, 4096) == XTC_XIR_PUBLICATION_OK);
    file_is(path, payload, sizeof(payload) - 1); directory_holds(1);
    write_file(path, "OLD");
    CHECK(replace(path, true, 4096) == XTC_XIR_PUBLICATION_OK);
    file_is(path, payload, sizeof(payload) - 1); directory_holds(1);
    write_file(path, "OLD");
    CHECK(replace(path, false, 4096) == XTC_XIR_PUBLICATION_OK);
    file_is(path, "OLD", 3); directory_holds(1);
    CHECK(replace(path, true, 3) == XTC_XIR_PUBLICATION_BUDGET);
    file_is(path, "OLD", 3); directory_holds(1);
    CHECK(!unlink(path));
}
/* Links, directories and missing parents are refused without side effects. */
static void refusals(void) {
    char path[4200], target[4200], missing[4200];
    path_in(path, sizeof(path), "entry"); path_in(target, sizeof(target), "target");
    CHECK(!mkdir(path, 0700));
    CHECK(replace(path, true, 4096) == XTC_XIR_PUBLICATION_UNSUPPORTED);
    directory_holds(1); CHECK(!rmdir(path));
    write_file(target, "KEEP"); CHECK(!symlink("target", path));
    CHECK(replace(path, true, 4096) == XTC_XIR_PUBLICATION_UNSUPPORTED);
    file_is(target, "KEEP", 4); directory_holds(2);
    CHECK(!unlink(path)); CHECK(!unlink(target));
    path_in(missing, sizeof(missing), "absent/out.c");
    CHECK(replace(missing, true, 4096) == XTC_XIR_PUBLICATION_UNRESOLVED);
    directory_holds(0);
    static const char *const invalid[] = {"", "dir/", ".", "..", "dir/..", "bad\xff.c"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(replace(invalid[i], true, 4096) == XTC_XIR_PUBLICATION_INVALID);
    char longer[5000]; memset(longer, 'a', sizeof(longer) - 1); longer[sizeof(longer) - 1] = 0;
    CHECK(replace(longer, true, 4096) == XTC_XIR_PUBLICATION_BUDGET);
}
/* Publication owns its memory independently of the caller's resource handle. */
static void lifetime(void) {
    char path[4200]; path_in(path, sizeof(path), "late.c");
    XrCompileResources *resources = NULL; XtcXirPublication *owner = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XtcXirPublicationRequest request = {path, {4096, 8, 4096}, false};
    CHECK(xtc_xir_publication_new(resources, &request, &owner) == XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_write(owner, payload, sizeof(payload) - 1) == XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_phase(owner) == XTC_XIR_PUBLICATION_READY);
    xr_compile_resources_release(resources);
    CHECK(xtc_xir_publication_commit(owner) == XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_phase(owner) == XTC_XIR_PUBLICATION_PUBLISHED);
    CHECK(xtc_xir_publication_close(&owner) == XTC_XIR_PUBLICATION_OK && !owner);
    file_is(path, payload, sizeof(payload) - 1); directory_holds(1); CHECK(!unlink(path));
}
/* Native executables are published with execute permission under the umask;
 * generated C keeps the ordinary 0666 creation mode. */
static void modes(void) {
    char path[4200]; path_in(path, sizeof(path), "program");
    mode_t previous = umask(022);
    for (int executable = 0; executable < 2; ++executable) {
        XrCompileResources *resources = NULL; XtcXirPublication *owner = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XtcXirPublicationRequest request = {path, {4096, 8, 4096}, executable != 0};
        CHECK(xtc_xir_publication_new(resources, &request, &owner) == XTC_XIR_PUBLICATION_OK);
        CHECK(xtc_xir_publication_write(owner, payload, sizeof(payload) - 1) == XTC_XIR_PUBLICATION_OK);
        CHECK(xtc_xir_publication_commit(owner) == XTC_XIR_PUBLICATION_OK);
        CHECK(xtc_xir_publication_close(&owner) == XTC_XIR_PUBLICATION_OK);
        xr_compile_resources_release(resources);
        struct stat information; CHECK(!stat(path, &information));
        CHECK((information.st_mode & 0777) == (executable ? 0755u : 0644u));
        CHECK(!unlink(path));
    }
    umask(previous);
}
int main(int argc, char **argv) {
    CHECK(argc == 2 && strlen(argv[1]) + 1 < sizeof(root));
    memcpy(root, argv[1], strlen(argv[1]) + 1);
    CHECK(!mkdir(root, 0700) || errno == EEXIST);
    directory_holds(0);
    replacement(); refusals(); lifetime(); modes();
    CHECK(!rmdir(root));
    puts("publication posix: ok");
    return 0;
}
