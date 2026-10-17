/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_handler_types.inc.c - Source-owned error selector correspondence
 */
typedef struct SourceHandlerRecipe {
    struct SourceHandlerRecipe *next, *parent;
    uint32_t identity, error, propagation;
    XrXirSourceDebugSelector *selectors;
    uint32_t selector_count;
} SourceHandlerRecipe;

typedef struct SourceRouteRecipe {
    struct SourceRouteRecipe *next;
    SourceHandlerRecipe *handler;
    uint32_t instruction;
} SourceRouteRecipe;
