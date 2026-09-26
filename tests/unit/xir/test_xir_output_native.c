/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_output_native.c - Native output without a VM or compiler
 *
 * KEY CONCEPT:
 *   Every sink publication matches independently specified bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_output_cases.h"
XR_DATA const XrXirCallEntry fixture_output_entries[1];
int main(void) { output_cases(fixture_output_entries); return 0; }
