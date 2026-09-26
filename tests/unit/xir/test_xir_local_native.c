/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_local_native.c - Local ownership through generated native entries
 *
 * KEY CONCEPT:
 *   Runtime-only execution checks independent expected bytes and release.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_local_cases.h"
XR_DATA const XrXirCallEntry fixture_local_entries[];
int main(void) { local_cases(fixture_local_entries); numeric_cleanup(fixture_local_entries + 1); puts("Native local ownership passed"); return 0; }
