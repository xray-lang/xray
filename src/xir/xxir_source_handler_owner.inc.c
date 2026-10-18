/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_handler_owner.inc.c - Own bounded error selector observations
 */
#include "xxir_source_handler_validate.inc.c"
static void source_debug_handler_copy(SourceQueryCopy *copy,const XrXirSourceDebugFunction *input,
    XrXirSourceDebugFunction *output) {
    if (input->route_count) output->routes=query_copy(copy,input->routes,input->route_count,sizeof(*input->routes));
    if (!input->handler_count) return;
    XrXirSourceDebugHandler *handlers=query_copy(copy,input->handlers,input->handler_count,sizeof(*handlers));
    output->handlers=handlers;
    for (uint32_t h=0;handlers && h<input->handler_count && copy->status==XR_XIR_OK;++h)
        handlers[h].selectors=query_copy(copy,input->handlers[h].selectors,input->handlers[h].selector_count,sizeof(*handlers[h].selectors));
}
