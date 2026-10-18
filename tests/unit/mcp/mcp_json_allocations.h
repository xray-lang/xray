#ifndef MCP_JSON_ALLOCATIONS_H
#define MCP_JSON_ALLOCATIONS_H
#include "base/xmalloc.h"
void *mcp_fault_malloc(size_t);
void *mcp_fault_calloc(size_t,size_t);
void *mcp_fault_realloc(void *,size_t);
void mcp_fault_free(void *);
char *mcp_fault_strdup(const char *);
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#undef xr_strdup
#define xr_malloc mcp_fault_malloc
#define xr_calloc mcp_fault_calloc
#define xr_realloc mcp_fault_realloc
#define xr_free mcp_fault_free
#define xr_strdup mcp_fault_strdup
#endif
