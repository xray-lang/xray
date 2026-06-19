/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_native.h - Native AOT-binary debugging bridge (DAP -> lldb-dap)
 *
 * KEY CONCEPT:
 *   The in-process DAP controller (xdap_controller.*) debugs the bytecode
 *   interpreter via debug hooks. A `.xr` compiled to a native executable
 *   (xray build --native -g) carries standard DWARF that already maps back
 *   to `.xr` source lines, so the most robust and maintainable way to debug
 *   it is to drive the toolchain's own debugger.
 *
 *   xdap_native_run spawns `lldb-dap` (LLVM's Debug Adapter Protocol server)
 *   and proxies the DAP byte stream between the editor and lldb-dap. The only
 *   transformation is on the `launch` request: when the requested `program`
 *   is a `.xr` file, the bridge compiles it to a temporary `-g` native binary
 *   (reusing `xray build --native -g`) and rewrites `program` to that binary,
 *   so the editor can "launch" a `.xr` and transparently get source-level
 *   native debugging. Every other message is forwarded verbatim, which keeps
 *   DAP sequence numbers consistent without any remapping.
 *
 * WHY DELEGATE INSTEAD OF REIMPLEMENT:
 *   lldb-dap is a maintained, correct DAP server. Reusing it (rather than
 *   reimplementing DAP <-> lldb translation) follows the same toolchain-reuse
 *   principle as the AOT debug-info path: least code, most stable.
 */

#ifndef XDAP_NATIVE_H
#define XDAP_NATIVE_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run the native debugging bridge.
 *
 *   in_fd            fd to read DAP messages from the editor (stdin in stdio mode)
 *   out_fd           fd to write DAP messages to the editor (stdout in stdio mode)
 *   self_exe         path to the running xray executable, used to compile
 *                    `.xr` launch targets (`<self_exe> build --native -g ...`);
 *                    may be NULL, in which case "xray" is resolved via PATH.
 *   debugger_override optional explicit path to lldb-dap; may be NULL, in which
 *                    case XRAY_LLDB_DAP and a small set of well-known locations
 *                    (plus PATH) are searched.
 *
 * Returns 0 on a clean session, non-zero on a setup failure (e.g. lldb-dap
 * could not be located or spawned). Blocks until either side closes.
 */
XR_FUNC int xdap_native_run(int in_fd, int out_fd, const char *self_exe,
                            const char *debugger_override);

#ifdef __cplusplus
}
#endif

#endif  // XDAP_NATIVE_H
