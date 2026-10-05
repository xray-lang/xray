/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_native20_main.c - Fixed selection for each independent executable
 */
#include <stdbool.h>
#include <string.h>
int atomic_native20_run(unsigned mode,bool compiler);
int main(int argc,char **argv) {
    if(argc!=1 && !(argc==2 && !strcmp(argv[1],"--compiler")))return 97;
    return atomic_native20_run(XR_ATOMIC_CASE,argc==2);
}
