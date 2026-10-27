/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_fingerprint_owner.c - Domain-separated source identity vectors
 */
#include "module/xmodule_fingerprint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void vector(const char *source, const char *expected) {
    XrFingerprint output = {{0}};
    xr_module_source_fingerprint(source, &output);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(output.bytes); ++i) {
        REQUIRE(hex[output.bytes[i] >> 4] == expected[2*i]);
        REQUIRE(hex[output.bytes[i] & 15] == expected[2*i+1]);
    }
}
int main(void) {
    vector("", "9e9510f365f323881aab67331161f000320a3a19c5ad5d74f80c7b0b05a5e38a");
    vector("abc", "20a093840eaaf0d6cc3191a5557bdfd186621ad5e98738c8e49acd0b06c12c0e");
    vector("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "648e5b4c87123463059752a7dda1e5fa6584fa7c0f3b5b77edc001e7045c9a64");
    vector("module\\nconst answer = 42\\n", "d0542d306e19b9bed66f6fdfbc1cf1fed222b84941dfbf295913dbedc3a4b92a");
    vector("脚本🚀", "207af19587af1a4f23b2d39af949ab295f4efa538a9edc68dd07361d6cb0cfc1");
    XrFingerprint empty = {{0}}, absent = {{0}}, short_text = {{0}}, long_text = {{0}};
    xr_module_source_fingerprint("", &empty);
    xr_module_source_fingerprint(NULL, &absent);
    REQUIRE(!memcmp(&empty, &absent, sizeof(empty)));
    xr_module_source_fingerprint("abc", &short_text);
    xr_module_source_fingerprint("abc\n", &long_text);
    REQUIRE(memcmp(&short_text, &long_text, sizeof(short_text)));
    xr_module_source_fingerprint(NULL, NULL);
    puts("Module fingerprint: independent SHA-256 domain, length and UTF-8 vectors passed");
    return 0;
}
