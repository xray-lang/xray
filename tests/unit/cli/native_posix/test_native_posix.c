/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_posix.c - POSIX native build owners fail closed
 *
 * KEY CONCEPT:
 *   Each owner behind `xray build` on macOS/Linux is driven directly: the
 *   private workspace, descriptor-walked leases and the process group that
 *   runs every command. Every case also returns all memory.
 */
#include "app/toolchain/xtc_xir_workspace.h"
#include "app/toolchain/xtc_xir_file_lease.h"
#include "app/toolchain/xtc_process.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line=%d %s errno=%d\n", __LINE__, #c, errno); exit(90); } } while (0)

static XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
static char root[PATH_MAX];

static XrCompileResources *ledger(XrCompileResourceStats *empty) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(resources, empty) == XR_COMPILE_RESOURCE_OK);
    return resources;
}
static void ledger_done(XrCompileResources *resources, const XrCompileResourceStats *empty) {
    XrCompileResourceStats closed = {0};
    CHECK(xr_compile_resources_stats(resources, &closed) == XR_COMPILE_RESOURCE_OK);
    CHECK(closed.held_bytes == empty->held_bytes);
    xr_compile_resources_release(resources);
}
static void path_in(char *out, const char *base, const char *leaf) {
    CHECK((size_t)snprintf(out, PATH_MAX, "%s/%s", base, leaf) < PATH_MAX);
}
static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file); CHECK(fwrite(text, 1, strlen(text), file) == strlen(text)); CHECK(!fclose(file));
}
static unsigned entries(const char *directory) {
    DIR *stream = opendir(directory); CHECK(stream);
    unsigned count = 0; struct dirent *entry;
    while ((entry = readdir(stream))) count += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
    CHECK(!closedir(stream)); return count;
}

/* Creation is private; one-step cleanup removes nested content but never
 * follows a link out of the tree, and refuses a root replaced by another. */
static void workspace(void) {
    char parent[PATH_MAX], outside[PATH_MAX], kept[PATH_MAX], path[PATH_MAX];
    path_in(parent, root, "workspace"); path_in(outside, root, "outside");
    CHECK(!mkdir(parent, 0700) && !mkdir(outside, 0700));
    path_in(kept, outside, "kept.txt"); write_file(kept, "KEEP");
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest relative = {"relative/parent", {4096, 8, 4096}};
    CHECK(xtc_xir_workspace_new(resources, &relative, &owner) == XTC_XIR_WORKSPACE_INVALID && !owner);
    XtcXirWorkspaceRequest request = {parent, {4096, 8, 4096}};
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_INVALID);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner);
    struct stat info;
    CHECK(!stat(paths->root, &info) && S_ISDIR(info.st_mode) && (info.st_mode & 077) == 0);
    CHECK(entries(paths->root) == 2 && entries(paths->input) == 0 && entries(paths->output) == 0);
    path_in(path, paths->output, "a"); CHECK(!mkdir(path, 0700));
    path_in(path, paths->output, "a/b"); CHECK(!mkdir(path, 0700));
    path_in(path, paths->output, "a/b/file"); write_file(path, "x");
    path_in(path, paths->output, "a/link-file"); CHECK(!symlink(kept, path));
    path_in(path, paths->output, "a/b/link-dir"); CHECK(!symlink(outside, path));
    for (unsigned i = 0; i < 40; ++i) {
        char name[32]; snprintf(name, sizeof(name), "f%u", i);
        path_in(path, paths->input, name); write_file(path, name);
    }
    unsigned steps = 0; XtcXirWorkspaceStatus closed;
    while ((closed = xtc_xir_workspace_close(&owner, 1)) == XTC_XIR_WORKSPACE_PENDING) CHECK(++steps < 10000);
    CHECK(closed == XTC_XIR_WORKSPACE_OK && !owner && steps > 40);
    CHECK(entries(parent) == 0 && entries(outside) == 1);
    CHECK(!stat(kept, &info) && info.st_size == 4);
    /* A directory swapped in under the private name is never removed. */
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    paths = xtc_xir_workspace_paths(owner);
    char moved[PATH_MAX], impostor[PATH_MAX];
    CHECK((size_t)snprintf(moved, sizeof(moved), "%s.moved", paths->root) < sizeof(moved));
    CHECK(!rename(paths->root, moved) && !mkdir(paths->root, 0700));
    path_in(impostor, paths->root, "foreign"); write_file(impostor, "FOREIGN");
    CHECK(xtc_xir_workspace_close(&owner, 1024) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH && owner);
    CHECK(!stat(impostor, &info));
    CHECK(!unlink(impostor) && !rmdir(paths->root) && !rename(moved, paths->root));
    CHECK(xtc_xir_workspace_close(&owner, 1024) == XTC_XIR_WORKSPACE_OK && !owner && entries(parent) == 0);
    ledger_done(resources, &empty);
}
/* Leases name the canonical file; a later read must find the same bytes. */
static void leases(void) {
    char file[PATH_MAX], link[PATH_MAX], fifo[PATH_MAX];
    path_in(file, root, "leased.txt"); path_in(link, root, "leased-link"); path_in(fifo, root, "fifo");
    write_file(file, "ORIGINAL"); CHECK(!symlink(file, link)); CHECK(!mkfifo(fifo, 0600));
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    XtcXirFileLease *lease = NULL, *directory = NULL, *refused = NULL;
    CHECK(xtc_xir_file_lease_open(resources, link, &lease) == XR_XIR_TARGET_OK);
    char canonical[PATH_MAX]; CHECK(realpath(file, canonical));
    const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(lease);
    CHECK(!strcmp(facts->path, canonical) && facts->length == 8);
    void *bytes = NULL; size_t length = 0;
    CHECK(xtc_xir_file_lease_read(lease, 64, &bytes, &length) == XR_XIR_TARGET_OK);
    CHECK(length == 8 && !memcmp(bytes, "ORIGINAL", 8));
    xr_compile_resources_free(bytes); bytes = NULL; length = 0;
    CHECK(xtc_xir_file_lease_read(lease, 4, &bytes, &length) == XR_XIR_TARGET_BUDGET && !bytes);
    xtc_xir_file_lease_free(lease); lease = NULL;
    CHECK(xtc_xir_file_lease_open(resources, file, &lease) == XR_XIR_TARGET_OK);
    write_file(file, "REPLACED");
    CHECK(xtc_xir_file_lease_read(lease, 64, &bytes, &length) == XR_XIR_TARGET_INVALID && !bytes);
    xtc_xir_file_lease_free(lease);
    CHECK(xtc_xir_file_lease_open(resources, fifo, &refused) == XR_XIR_TARGET_INVALID && !refused);
    CHECK(xtc_xir_file_lease_directory_open(resources, file, &refused) == XR_XIR_TARGET_UNRESOLVED && !refused);
    CHECK(xtc_xir_file_lease_directory_open(resources, root, &directory) == XR_XIR_TARGET_OK);
    CHECK(xtc_xir_file_lease_directory_facts(directory) && !xtc_xir_file_lease_facts(directory));
    xtc_xir_file_lease_free(directory);
    CHECK(!unlink(file) && !unlink(link) && !unlink(fifo));
    ledger_done(resources, &empty);
}
/* Darwin reports EPERM when a group holds only its unreaped leader; every
 * completed command must still read as a successful run. */
static void processes(void) {
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    for (unsigned i = 0; i < 64; ++i) {
        XrProcessSpec spec; XrToolchainProcess *process = NULL; XrProcessResult result = {0};
        xtc_process_spec_init(&spec, "/bin/sh", 30000);
        spec.environment_source = XTC_PROCESS_ENV_EXPLICIT; spec.cwd = "/";
        spec.completion_policy = i % 2 ? XR_PROC_COMPLETE_ROOT : XR_PROC_COMPLETE_TREE;
        spec.argv[1] = "-c"; spec.argv[2] = i % 4 < 2 ? "exit 0" : "exit 3";
        CHECK(xtc_process_prepare(resources, &spec, &process) == XTC_PROCESS_OK);
        CHECK(xtc_process_run(process, NULL, NULL, &result) == XTC_PROCESS_OK);
        CHECK(result.exit_code == (i % 4 < 2 ? 0 : 3));
        xtc_process_result_free(&result); xtc_process_free(process);
    }
    ledger_done(resources, &empty);
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <scratch-root>\n", argv[0]); return 2; }
    char scratch[PATH_MAX];
    CHECK((size_t)snprintf(scratch, sizeof(scratch), "%s.%ld", argv[1], (long)getpid()) < sizeof(scratch));
    CHECK(!mkdir(scratch, 0700) && realpath(scratch, root));
    umask(022);
    workspace(); leases(); processes();
    char path[PATH_MAX];
    path_in(path,root,"outside/kept.txt"); CHECK(!unlink(path));
    path_in(path,root,"outside"); CHECK(!rmdir(path));
    path_in(path,root,"workspace"); CHECK(!rmdir(path));
    CHECK(!rmdir(root));
    puts("POSIX workspace, leases and processes PASS");
    return 0;
}
