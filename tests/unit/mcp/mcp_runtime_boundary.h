/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mcp_runtime_boundary.h - Arm faults after source compilation has completed
 */
#ifndef MCP_RUNTIME_BOUNDARY_H
#define MCP_RUNTIME_BOUNDARY_H
#include "xir/xxir_program.h"
XR_FUNC XrXirCallStatus mcp_runtime_instance_new(XrXirProgram *program,
    const XrXirInstanceConfig *config, XrXirInstance **output);
XR_FUNC XrXirCallStatus mcp_runtime_instance_wait_request(XrXirInstance *instance,
    uint64_t epoch, uint64_t wake, XrXirWaitRequest *output);
XR_FUNC XrXirCallStatus mcp_runtime_instance_resume(XrXirInstance *instance,
    uint64_t epoch, uint64_t wake);
#define xr_xir_instance_new mcp_runtime_instance_new
#define xr_xir_instance_wait_request mcp_runtime_instance_wait_request
#define xr_xir_instance_resume mcp_runtime_instance_resume
#endif // MCP_RUNTIME_BOUNDARY_H
