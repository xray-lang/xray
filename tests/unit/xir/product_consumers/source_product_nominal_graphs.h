/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_nominal_graphs.h - Exact structural source consumer inputs
 */
#ifndef SOURCE_PRODUCT_NOMINAL_GRAPHS_H
#define SOURCE_PRODUCT_NOMINAL_GRAPHS_H
typedef struct SourceProductNominalFixture {
    const char *name, *root, *file;
    unsigned classes;
} SourceProductNominalFixture;
int xr_source_product_nominal_main(const SourceProductNominalFixture *, int, char **);
#endif // SOURCE_PRODUCT_NOMINAL_GRAPHS_H
