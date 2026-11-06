/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdlib_source_owner.c - Source metadata without runtime binders
 */
#include "module/xstdlib_embedded.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)

static void source_identity(const char *name) {
    const XrStdlibSourceDescriptor *descriptor=xr_stdlib_source_descriptor(name);
    const char *source=xr_get_embedded_stdlib(name);
    CHECK(descriptor && !strcmp(descriptor->name,name) && source && descriptor->source==source);
    CHECK(descriptor==xr_stdlib_source_descriptor(name) && source==xr_get_embedded_stdlib(name));
    uint8_t digest[32]; size_t length=strlen(source);
    xr_sha256((const uint8_t *)source,length,digest);
    printf("SOURCE %s %zu ",name,length);
    for(unsigned i=0;i<sizeof(digest);++i)printf("%02x",(unsigned)digest[i]);
    putchar('\n');
}
int main(void) {
    source_identity("io");source_identity("path");
    const XrStdlibSourceDescriptor *io=xr_stdlib_source_descriptor("io");
    CHECK(io && io->has_native_entries);
    CHECK(!xr_stdlib_source_descriptor("?") && !xr_get_embedded_stdlib("?"));
    CHECK(!xr_stdlib_source_descriptor(NULL) && !xr_get_embedded_stdlib(NULL));
    puts("source metadata: immutable borrowed descriptors, original source bytes, no runtime binder linkage PASS");
    return 0;
}
