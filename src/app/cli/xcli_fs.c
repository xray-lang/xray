/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_fs.c - Unified file system operations for CLI commands
 *
 * KEY CONCEPT:
 *   Consolidates file I/O and directory traversal that was previously
 *   scattered across xcli_utils.c, xcmd_check.c, xcmd_fmt.c, xcmd_test.c.
 *   All CLI commands now use the same ignore rules and traversal logic.
 */

#include "xcli_fs.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_dir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../os/os_fs.h"
#include <limits.h>
#include <errno.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

/* ========== Default Ignore Rules ========== */

/* Directories always skipped during recursive traversal.
 * This is the single source of truth for CLI ignore behavior. */
static const char *const s_default_ignore_dirs[] = {
    ".git", ".svn", ".hg", "build", "build-asan", "build-release", "node_modules", ".xray", NULL};

static bool is_ignored_dir(const char *name, const XrCliWalkOpts *opts) {
    XR_DCHECK(name != NULL, "name must not be NULL");
    XR_DCHECK(opts != NULL, "opts must not be NULL");

    /* Hidden directories (starting with .) */
    if (opts->skip_hidden && name[0] == '.')
        return true;

    /* Underscore-prefixed directories */
    if (opts->skip_underscore && name[0] == '_')
        return true;

    /* Check hardcoded ignore list */
    for (const char *const *p = s_default_ignore_dirs; *p; p++) {
        if (strcmp(name, *p) == 0)
            return true;
    }

    /* Check extra ignore patterns (simple name match for now) */
    if (opts->extra_ignore) {
        for (const char *const *p = opts->extra_ignore; *p; p++) {
            if (strcmp(name, *p) == 0)
                return true;
        }
    }

    return false;
}

/* ========== File List ========== */

void xr_cli_filelist_init(XrCliFileList *fl) {
    XR_DCHECK(fl != NULL, "fl must not be NULL");
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

void xr_cli_filelist_free(XrCliFileList *fl) {
    if (!fl)
        return;
    for (int i = 0; i < fl->count; i++) {
        xr_free(fl->paths[i]);
    }
    xr_free(fl->paths);
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

void xr_cli_filelist_add(XrCliFileList *fl, const char *path) {
    XR_DCHECK(fl != NULL, "fl must not be NULL");
    XR_DCHECK(path != NULL, "path must not be NULL");

    if (fl->count >= XR_CLI_MAX_FILES)
        return;

    if (fl->count >= fl->capacity) {
        int new_cap = fl->capacity == 0 ? 64 : fl->capacity * 2;
        char **tmp = (char **) xr_realloc(fl->paths, (size_t) new_cap * sizeof(char *));
        if (!tmp)
            return;
        fl->paths = tmp;
        fl->capacity = new_cap;
    }

    size_t len = strlen(path);
    char *copy = (char *) xr_malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, path, len + 1);
    fl->paths[fl->count++] = copy;
}

static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **) a, *(const char **) b);
}

void xr_cli_filelist_sort(XrCliFileList *fl) {
    XR_DCHECK(fl != NULL, "fl must not be NULL");
    if (fl->count > 1) {
        qsort(fl->paths, (size_t) fl->count, sizeof(char *), cmp_strings);
    }
}

/* ========== Directory Traversal ========== */

XrCliWalkOpts xr_cli_walk_defaults(void) {
    return (XrCliWalkOpts){
        .xr_only = true,
        .skip_hidden = true,
        .skip_underscore = false,
        .extra_ignore = NULL,
    };
}

static void walk_recursive(const char *dirpath, const XrCliWalkOpts *opts, XrCliFileList *fl) {
    XrDirIter *it = xr_dir_open(dirpath);
    if (!it)
        return;

    char filepath[XR_CLI_PATH_MAX];
    XrDirEntry e;

    while (xr_dir_next(it, &e)) {
        int n = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, e.name);
        if (n < 0 || (size_t) n >= sizeof(filepath))
            continue;

        if (e.is_dir) {
            if (!is_ignored_dir(e.name, opts)) {
                walk_recursive(filepath, opts, fl);
            }
        } else {
            if (opts->xr_only && !xr_cli_is_xr_file(e.name)) {
                continue;
            }
            xr_cli_filelist_add(fl, filepath);
        }
    }

    xr_dir_close(it);
}

int xr_cli_collect_files(const char *path, const XrCliWalkOpts *opts, XrCliFileList *fl) {
    XR_DCHECK(path != NULL, "path must not be NULL");
    XR_DCHECK(fl != NULL, "fl must not be NULL");

    XrCliWalkOpts defaults = xr_cli_walk_defaults();
    if (!opts)
        opts = &defaults;

    XrFsStat st;
    if (xr_fs_stat(path, &st) != 0)
        return -1;

    if (st.kind == XR_FS_FILE) {
        /* Single file — add directly (ignore xr_only filter for explicit files) */
        xr_cli_filelist_add(fl, path);
        return 0;
    }

    if (st.kind == XR_FS_DIR) {
        walk_recursive(path, opts, fl);
        xr_cli_filelist_sort(fl);
        return 0;
    }

    return -1;
}

/* ========== File I/O ========== */

char *xr_cli_read_file(const char *path) {
    XR_DCHECK(path != NULL, "path must not be NULL");

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    char *content = (char *) xr_malloc((size_t) size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(content, 1, (size_t) size, f);
    content[nread] = '\0';
    fclose(f);

    return content;
}

char *xr_cli_read_stdin(void) {
    size_t capacity = 4096;
    size_t length = 0;
    char *buf = (char *) xr_malloc(capacity);
    if (!buf)
        return NULL;

    size_t n;
    while ((n = fread(buf + length, 1, capacity - length - 1, stdin)) > 0) {
        length += n;
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *tmp = (char *) xr_realloc(buf, capacity);
            if (!tmp) {
                xr_free(buf);
                return NULL;
            }
            buf = tmp;
        }
    }
    buf[length] = '\0';
    return buf;
}

int xr_cli_write_file(const char *path, const char *content) {
    XR_DCHECK(path != NULL, "path must not be NULL");
    XR_DCHECK(content != NULL, "content must not be NULL");

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    return written == len ? 0 : -1;
}

int xr_cli_write_file_atomic(const char *path, const char *content) {
    XR_DCHECK(path != NULL, "path must not be NULL");
    XR_DCHECK(content != NULL, "content must not be NULL");

    /* Write to a temporary file in the same directory, then rename.
     * This avoids partial writes corrupting the original. */
    char tmp_path[XR_CLI_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.xrtmp", path);
    if (n < 0 || (size_t) n >= sizeof(tmp_path))
        return -1;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    if (fflush(f) != 0 || written != len) {
        fclose(f);
        xr_fs_remove(tmp_path);
        return -1;
    }
    fclose(f);

    /* Preserve original permissions if file exists */
    struct stat st;
    if (stat(path, &st) == 0) {
        chmod(tmp_path, st.st_mode);
    }

    if (rename(tmp_path, path) != 0) {
        xr_fs_remove(tmp_path);
        return -1;
    }

    return 0;
}

/* ========== Path Queries ========== */

bool xr_cli_file_exists(const char *path) {
    return xr_fs_exists(path);
}

bool xr_cli_is_directory(const char *path) {
    return xr_fs_is_dir(path);
}

bool xr_cli_is_xr_file(const char *filename) {
    if (!filename)
        return false;
    size_t len = strlen(filename);
    if (len < 4)
        return false;
    return strcmp(filename + len - 3, ".xr") == 0;
}

/* ========== Safe Parsing Helpers ========== */

bool xr_cli_parse_int(const char *str, int *out) {
    if (!str || !*str)
        return false;

    char *endptr;
    long val = strtol(str, &endptr, 10);

    if (endptr == str || *endptr != '\0')
        return false;
    if (val < INT_MIN || val > INT_MAX)
        return false;

    *out = (int) val;
    return true;
}

bool xr_cli_parse_port(const char *str, int *out) {
    int val;
    if (!xr_cli_parse_int(str, &val))
        return false;
    if (val < 0 || val > 65535)
        return false;
    *out = val;
    return true;
}
