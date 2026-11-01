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
 *   private workspace, descriptor-walked leases, the compiler and linker
 *   report grammars, ELF64/Mach-O admission facts, driver discovery and the
 *   process group that runs every command. Every case also returns all memory.
 */
#include "app/toolchain/xtc_xir_workspace.h"
#include "app/toolchain/xtc_xir_file_lease.h"
#include "app/toolchain/xtc_dependencies.h"
#include "app/toolchain/xtc_xir_native_image.h"
#include "app/toolchain/xtc_xir_local_toolchain.h"
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
static char root[PATH_MAX], sdk[PATH_MAX];

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
static XrXirTargetStatus report(XrDependencyFormat format, const char *text, size_t length,
    uint32_t *count, uint32_t *headers) {
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    static const char *const argv[] = {"/usr/bin/cc", "-c"};
    XrDependencyInput input = {format, (const uint8_t *)text, length, argv, 2};
    XrDependencyLimits bounds = {1 << 20, 4096, 64};
    XrDependencyFacts *facts = NULL;
    XrXirTargetStatus status = xtc_dependencies_parse(resources, &input, bounds, &facts);
    *count = xtc_dependencies_count(facts); *headers = 0;
    for (uint32_t i = 0; i < *count; ++i) *headers += xtc_dependencies_record(facts, i)->kind == XR_DEPENDENCY_HEADER;
    xtc_dependencies_free(facts); ledger_done(resources, &empty);
    return status;
}
#define REPORT(format, text, status, expected) do { uint32_t n = 0, h = 0; \
    CHECK(report(format, text, sizeof(text) - 1, &n, &h) == (status)); CHECK(n == (expected)); } while (0)
/* The closed spellings of cc -MD, GNU ld/LLD and ld64 reports. */
static void reports(void) {
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/generated.o: \\\n  /sdk/SDKSettings.json \\\n  /w/i/generated.c \\\n"
        "  /w/i/dir\\ with\\ space/x.h \\\n  /s/src/a/../b.h\n", XR_XIR_TARGET_OK, 4);
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/a.o: /w/i/a.c /usr/include/stdio.h\n", XR_XIR_TARGET_OK, 2);
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/a.o: relative.c\n", XR_XIR_TARGET_INVALID, 0);
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/a.o: /w/$(HOME)/a.c\n", XR_XIR_TARGET_UNSUPPORTED, 0);
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/a.o: /w/i/a.c\r\n", XR_XIR_TARGET_UNSUPPORTED, 0);
    REPORT(XR_DEPENDENCY_POSIX_MAKE, "/w/o/a.o: /w/i/a.c\n/w/o/b.o: /w/i/b.c\n", XR_XIR_TARGET_UNSUPPORTED, 0);
    REPORT(XR_DEPENDENCY_POSIX_LINK_MAKE, "/w/o/program: \\\n  /w/o/a.o \\\n  /usr/lib/libc.so\n"
        "\n/w/o/a.o:\n\n/usr/lib/libc.so:\n", XR_XIR_TARGET_OK, 2);
    REPORT(XR_DEPENDENCY_POSIX_LINK_MAKE, "/w/o/program: \\\n  /w/o/a.o \\\n  /usr/lib/libc.so\n"
        "\n/usr/lib/libc.so:\n\n/w/o/a.o:\n", XR_XIR_TARGET_INVALID, 0);
    REPORT(XR_DEPENDENCY_DARWIN_LD_INFO, "\0@(#)PROGRAM:ld PROJECT:ld-1\n\0\x10/w/o/a.o\0\x11/sdk/missing.tbd\0"
        "\x10/sdk/libSystem.tbd\0\x40/w/o/program\0", XR_XIR_TARGET_OK, 3);
    REPORT(XR_DEPENDENCY_DARWIN_LD_INFO, "\x10/w/o/a.o\0\x40/w/o/program\0", XR_XIR_TARGET_INVALID, 0);
    REPORT(XR_DEPENDENCY_DARWIN_LD_INFO, "\0ld\0\x10/w/o/a.o\0\x41/w/o/x\0", XR_XIR_TARGET_UNSUPPORTED, 0);
    REPORT(XR_DEPENDENCY_DARWIN_LD_INFO, "\0ld\0\x10relative.o\0\x40/w/o/program\0", XR_XIR_TARGET_INVALID, 0);
    REPORT(XR_DEPENDENCY_DARWIN_LD_INFO, "\0ld\0\x10/w/o/a.o", XR_XIR_TARGET_INVALID, 0);
}
static bool defined_x86_64(void) {
#ifdef XR_ARCH_X86_64
    return true;
#else
    return false;
#endif
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
/* Minimal ELF64: header, PT_INTERP (optional) and one PF_X PT_LOAD. */
static size_t elf(uint8_t *image, uint16_t type, uint16_t machine, bool interpreter, uint32_t flags) {
    memset(image, 0, 512);
    memcpy(image, "\x7f" "ELF\x02\x01\x01", 7);
    put16(image + 16, type); put16(image + 18, machine); put32(image + 20, 1);
    put64(image + 24, 0x1100); put64(image + 32, 64); put16(image + 54, 56);
    put16(image + 56, interpreter ? 2 : 1);
    uint8_t *load = image + 64 + (interpreter ? 56 : 0);
    if (interpreter) put32(image + 64, 3);
    put32(load, 1); put32(load + 4, flags); put64(load + 8, 0); put64(load + 16, 0x1000);
    put64(load + 32, 512); put64(load + 40, 512);
    return 512;
}
/* Minimal Mach-O 64: one LC_SEGMENT_64 and the requested LC_MAIN count. */
static size_t macho(uint8_t *image, uint32_t cpu, uint32_t mains, uint32_t protection) {
    memset(image, 0, 512);
    put32(image, 0xfeedfacfu); put32(image + 4, cpu); put32(image + 12, 2);
    put32(image + 16, 1 + mains); put32(image + 20, 72 + 24 * mains);
    put32(image + 32, 0x19); put32(image + 36, 72);
    put64(image + 32 + 40, 0); put64(image + 32 + 48, 512); put32(image + 32 + 60, protection);
    for (uint32_t i = 0; i < mains; ++i) {
        uint8_t *main_command = image + 104 + 24 * i;
        put32(main_command, 0x80000028u); put32(main_command + 4, 24); put64(main_command + 8, 0x100);
    }
    return 512;
}
static void images(const char *self) {
    uint8_t image[512]; XtcXirNativeImage facts;
    const uint32_t x64 = XTC_XIR_ELF_MACHINE_X86_64, arm = XTC_XIR_ELF_MACHINE_AARCH64;
    CHECK(xtc_xir_native_image_parse(image, elf(image, 3, x64, true, 5), XTC_XIR_NATIVE_IMAGE_ELF64, x64, &facts) ==
        XTC_XIR_NATIVE_IMAGE_OK && facts.type == 3 && facts.entry == 0x1100);
    CHECK(xtc_xir_native_image_parse(image, elf(image, 2, arm, false, 5), XTC_XIR_NATIVE_IMAGE_ELF64, arm, &facts) ==
        XTC_XIR_NATIVE_IMAGE_OK);
    /* A shared library, foreign CPU or non-executable entry is not admitted. */
    CHECK(xtc_xir_native_image_parse(image, elf(image, 3, x64, false, 5), XTC_XIR_NATIVE_IMAGE_ELF64, x64, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    CHECK(xtc_xir_native_image_parse(image, elf(image, 3, arm, true, 5), XTC_XIR_NATIVE_IMAGE_ELF64, x64, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    CHECK(xtc_xir_native_image_parse(image, elf(image, 3, x64, true, 4), XTC_XIR_NATIVE_IMAGE_ELF64, x64, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    elf(image, 3, x64, true, 5); put16(image + 56, 60);
    CHECK(xtc_xir_native_image_parse(image, 512, XTC_XIR_NATIVE_IMAGE_ELF64, x64, &facts) == XTC_XIR_NATIVE_IMAGE_INVALID);
    const uint32_t mac = XTC_XIR_MACHO_CPU_ARM64;
    CHECK(xtc_xir_native_image_parse(image, macho(image, mac, 1, 5), XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) ==
        XTC_XIR_NATIVE_IMAGE_OK && facts.entry == 0x100);
    CHECK(xtc_xir_native_image_parse(image, macho(image, mac, 0, 5), XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    CHECK(xtc_xir_native_image_parse(image, macho(image, mac, 2, 5), XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    CHECK(xtc_xir_native_image_parse(image, macho(image, mac, 1, 1), XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) ==
        XTC_XIR_NATIVE_IMAGE_UNSUPPORTED);
    macho(image, mac, 1, 5); put32(image + 36, 4096);
    CHECK(xtc_xir_native_image_parse(image, 512, XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) == XTC_XIR_NATIVE_IMAGE_INVALID);
    CHECK(xtc_xir_native_image_parse(image, 16, XTC_XIR_NATIVE_IMAGE_MACHO64, mac, &facts) == XTC_XIR_NATIVE_IMAGE_INVALID);
    /* This test binary itself is a host executable the parser admits. It is
     * read into heap memory: reading an executable into its own file-backed
     * data pages can deadlock the Darwin kernel. */
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    XtcXirFileLease *lease = NULL; void *bytes = NULL; size_t length = 0;
    CHECK(xtc_xir_file_lease_open(resources, self, &lease) == XR_XIR_TARGET_OK);
    CHECK(xtc_xir_file_lease_read(lease, 64u << 20, &bytes, &length) == XR_XIR_TARGET_OK && length > 64);
#if defined(XR_OS_MACOS)
    uint32_t machine = defined_x86_64() ? XTC_XIR_MACHO_CPU_X86_64 : XTC_XIR_MACHO_CPU_ARM64;
    XtcXirNativeImageFormat format = XTC_XIR_NATIVE_IMAGE_MACHO64;
#else
    uint32_t machine = defined_x86_64() ? XTC_XIR_ELF_MACHINE_X86_64 : XTC_XIR_ELF_MACHINE_AARCH64;
    XtcXirNativeImageFormat format = XTC_XIR_NATIVE_IMAGE_ELF64;
#endif
    CHECK(xtc_xir_native_image_parse(bytes, length, format, machine, &facts) == XTC_XIR_NATIVE_IMAGE_OK);
    xr_compile_resources_free(bytes); xtc_xir_file_lease_free(lease); ledger_done(resources, &empty);
}
/* A stand-in driver answering the discovery probe with chosen macros. */
static void fake_driver(const char *path, const char *pointer, bool host) {
#ifdef XR_ARCH_X86_64
    const char *arch = host ? "__x86_64__" : "__aarch64__";
#else
    const char *arch = host ? "__aarch64__" : "__x86_64__";
#endif
#ifdef XR_OS_MACOS
    const char *system = "__MACH__";
#else
    const char *system = "__ELF__";
#endif
    FILE *file = fopen(path, "wb"); CHECK(file);
    fprintf(file, "#!/bin/sh\nfor a in \"$@\"; do if [ \"$a\" = -print-search-dirs ]; then\n"
        "echo 'programs: =/usr/bin'; echo 'libraries: =/usr/lib:/nonexistent/xray/lib'; exit 0; fi; done\n"
        "echo '#define __GNUC__ 9'; echo '#define __GNUC_MINOR__ 8'; echo '#define __GNUC_PATCHLEVEL__ 7'\n"
        "echo '#define __STDC_VERSION__ 201112L'; echo '#define __SIZEOF_POINTER__ %s'\n"
        "echo '#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__'; echo '#define %s 1'; echo '#define %s 1'\n"
        "echo '#include <...> search starts here:' >&2; echo ' /usr/include' >&2\n"
        "echo ' /nonexistent/xray/include' >&2; echo 'End of search list.' >&2\n", pointer, arch, system);
    CHECK(!fclose(file) && !chmod(path, 0755));
}
static XtcXirLocalStatus discover(const char *compiler, XtcXirLocalDiagnostic *diagnostic, uint32_t *provider) {
    XrCompileResourceStats empty; XrCompileResources *resources = ledger(&empty);
    XtcXirLocalToolchainRequest request = {compiler, sdk, NULL, 30000, 4u << 20, 1u << 20};
    XtcXirLocalToolchain *owner = NULL;
    XtcXirLocalStatus status = xtc_xir_local_toolchain_open(resources, &request, &owner, diagnostic);
    if (status == XTC_XIR_LOCAL_OK) {
        const XtcXirLocalToolchainView *view = xtc_xir_local_toolchain_view(owner);
        CHECK(view->compiler && view->compiler[0] == '/' && !strcmp(view->compiler, view->linker));
        CHECK(view->posix.path && view->posix.root_count && !view->library_count && view->workspace_parent);
        for (uint32_t i = 0; i < view->posix.root_count; ++i) {
            struct stat info; CHECK(!stat(view->posix.roots[i], &info) && S_ISDIR(info.st_mode));
        }
        *provider = view->posix.provider;
        if (*provider == XR_TOOLCHAIN_BINDING_PROVIDER_GCC && strstr(view->compiler, "fake"))
            CHECK(!strcmp(view->posix.version.file_text, "9.8.7"));
        xtc_xir_local_toolchain_free(owner);
    }
    ledger_done(resources, &empty);
    return status;
}
/* Discovery admits only a C11 driver for this host's CPU, OS and LP64 layout. */
static void discovery(void) {
    char good[PATH_MAX], narrow[PATH_MAX], foreign[PATH_MAX], missing[PATH_MAX], tools[PATH_MAX];
    path_in(tools, root, "tools"); CHECK(!mkdir(tools, 0700));
    path_in(good, tools, "fake-good-cc"); path_in(narrow, tools, "fake-narrow-cc");
    path_in(foreign, tools, "fake-foreign-cc"); path_in(missing, tools, "absent-cc");
    fake_driver(good, "8", true); fake_driver(narrow, "4", true); fake_driver(foreign, "8", false);
    XtcXirLocalDiagnostic diagnostic = {0}; uint32_t provider = 0;
    CHECK(discover(good, &diagnostic, &provider) == XTC_XIR_LOCAL_OK && provider == XR_TOOLCHAIN_BINDING_PROVIDER_GCC);
    CHECK(discover(narrow, &diagnostic, &provider) == XTC_XIR_LOCAL_UNSUPPORTED);
    CHECK(diagnostic.stage == XTC_XIR_LOCAL_ENVIRONMENT && diagnostic.domain == XTC_XIR_LOCAL_SELF);
    CHECK(discover(foreign, &diagnostic, &provider) == XTC_XIR_LOCAL_UNSUPPORTED);
    CHECK(discover(missing, &diagnostic, &provider) == XTC_XIR_LOCAL_UNRESOLVED);
    /* A bare name is searched in PATH; only absolute entries are consulted. */
    const char *saved = getenv("PATH"); char search[8192];
    CHECK((size_t)snprintf(search, sizeof(search), ".:%s:%s", tools, saved ? saved : "/usr/bin") < sizeof(search));
    CHECK(!setenv("PATH", search, 1));
    CHECK(discover("fake-good-cc", &diagnostic, &provider) == XTC_XIR_LOCAL_OK);
    CHECK(discover("absent-cc", &diagnostic, &provider) == XTC_XIR_LOCAL_UNRESOLVED);
    CHECK(!setenv("PATH", saved ? saved : "/usr/bin:/bin", 1));
    /* The host's default driver is discoverable where xray itself was built. */
    CHECK(discover(NULL, &diagnostic, &provider) == XTC_XIR_LOCAL_OK);
    CHECK(provider == XR_TOOLCHAIN_BINDING_PROVIDER_CLANG || provider == XR_TOOLCHAIN_BINDING_PROVIDER_GCC);
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
    if (argc != 3) { fprintf(stderr, "usage: %s <scratch-root> <runtime-sdk>\n", argv[0]); return 2; }
    char scratch[PATH_MAX];
    CHECK((size_t)snprintf(scratch, sizeof(scratch), "%s.%ld", argv[1], (long)getpid()) < sizeof(scratch));
    CHECK(!mkdir(scratch, 0700) && realpath(scratch, root) && realpath(argv[2], sdk));
    umask(022);
    char self[PATH_MAX]; CHECK(realpath(argv[0], self));
    workspace(); leases(); reports(); images(self); discovery(); processes();
    char command[PATH_MAX + 16];
    CHECK((size_t)snprintf(command, sizeof(command), "rm -rf '%s'", root) < sizeof(command) && !system(command));
    puts("POSIX workspace, leases, reports, images, discovery and processes PASS");
    return 0;
}
