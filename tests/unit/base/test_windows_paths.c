/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_windows_paths.c - UTF-8 filesystem and publication boundary checks
 */
#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "os/os_file_read.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <string.h>
static int verify_publication(const char *root) {
    char first[4096], final[4096], other[4096], current[4096], saved[4096];
    if (snprintf(first,sizeof(first),"%s/tmp-\xf0\x9f\x98\x80",root)<=0 ||
        snprintf(final,sizeof(final),"%s/final-\xe4\xb8\xad",root)<=0 ||
        snprintf(other,sizeof(other),"%s/other-\xe6\x96\x87",root)<=0) return 0;
    static const uint8_t data[]={1,2,3,4};
    if (xr_fs_write_new_file_sync(first,data,sizeof(data)) ||
        xr_fs_write_new_file_sync(first,data,sizeof(data))==0) return 0;
    uint8_t *read=NULL;size_t size=0;
    if (xr_fs_read_regular_file(first,3,&read,&size)==0 || read || size) return 0;
    if (xr_fs_read_regular_file(first,4,&read,&size) || size!=4 || memcmp(read,data,4)) return 0;
    xr_free(read);
    if (xr_fs_rename(first,final) || xr_fs_touch(final)) return 0;
    if (xr_fs_write_new_file_sync(other,data,sizeof(data))) return 0;
    if (xr_fs_publish_noreplace(other,final)!=XR_FS_PUBLISH_EXISTS || !xr_fs_exists(other)) return 0;
    if (xr_fs_publish_noreplace(other,first)!=XR_FS_PUBLISH_OK) return 0;
    if (!xr_fs_getcwd(saved,sizeof(saved)) || xr_fs_chdir(root)) return 0;
    int same=xr_fs_getcwd(current,sizeof(current)) && !strcmp(current,root);
    if (xr_fs_chdir(saved) || !same) return 0;
    if (xr_fs_remove(first) || xr_fs_remove(final)) return 0;
    return 1;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    size_t size = 0;
    char *root = xr_file_read_all(argv[1], "rb", &size);
    if (!root || !size) return 3;
    char *resolved = xr_realpath(root);
    char *path = xr_path_join(root, "main.xr");
    char *legacy = path ? xr_file_read_all(path, "rb", NULL) : NULL;
    XrFileBytes bytes = {0};
    XrFileReadStatus status = xr_file_read_under_root(root, "main.xr", 4096, &bytes);
    printf("{\"realpath_preserves_utf8\":%s,\"narrow_read_ok\":%s,\"rooted_read_status\":%d,\"rooted_bytes\":%zu}\n",
        resolved && !strcmp(root, resolved) ? "true" : "false", legacy ? "true" : "false", (int)status, bytes.size);
    int exact = resolved && !strcmp(root,resolved) && legacy && !strcmp(legacy,"print(41)\n") &&
        bytes.size == 10 && !memcmp(bytes.data,"print(41)\n",10);
    int published = verify_publication(root);
    printf("publication_and_cwd=%d\n",published);
    xr_free(bytes.data); xr_free(legacy); xr_free(path); xr_free(resolved); xr_free(root);
    return status == XR_FILE_READ_OK && published && exact ? 0 : 1;
}
