/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain_update.h - Owned persistent toolchain preference transactions
 */
#ifndef XCLI_TOOLCHAIN_UPDATE_H
#define XCLI_TOOLCHAIN_UPDATE_H
#include "xcli_toolchain_config.h"

typedef struct XrCliToolchainUpdate XrCliToolchainUpdate;
typedef enum XrCliToolchainUpdateStatus {
    XR_CLI_TOOLCHAIN_UPDATE_OK, XR_CLI_TOOLCHAIN_UPDATE_INVALID,
    XR_CLI_TOOLCHAIN_UPDATE_BUDGET, XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY,
    XR_CLI_TOOLCHAIN_UPDATE_UNRESOLVED, XR_CLI_TOOLCHAIN_UPDATE_IO,
    XR_CLI_TOOLCHAIN_UPDATE_UNSUPPORTED, XR_CLI_TOOLCHAIN_UPDATE_PENDING
} XrCliToolchainUpdateStatus;
typedef struct XrCliToolchainUpdateRequest {
    const char *path, *target, *compiler, *zig;
    XrToolchainSelector selector;
    /* NULL target resets all preferences and permits deleting corrupt input.
     * A targeted reset retains all other entries and policy. Reset forbids
     * compiler/zig arguments and requires the neutral AUTO selector. */
    bool reset;
} XrCliToolchainUpdateRequest;
typedef struct XrCliToolchainUpdateView {
    const char *path;
    XrCliToolchainUpdateStatus first_error;
    bool published, removed;
} XrCliToolchainUpdateView;

/* Windows implementation. Copies and validates the entire borrowed request on
 * the mandatory original ledger, without writing or locking files. The default
 * locator is resolved now. Failure preserves the initially NULL output. The
 * original caller state is retained and owns sticky resource failure across
 * construction, nested config reads and later retries on the same state. */
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_new(XrCompileState *,
    const XrCliToolchainUpdateRequest *, XrCliToolchainUpdate **);
/* Exactly once. Locks the persistent sibling lock file, then reads the latest
 * strict snapshot and atomically publishes a complete replacement. A reset-all
 * unlinks the configured regular-file entry. Neither operation promises crash
 * durability or CAS against writers that ignore the lock. The lock file stays
 * in place for cooperating processes. Failures preserve old config/output until
 * the actual commit; the owner retains cleanup responsibility on every result.
 * Calls and views require caller exclusion. No budget or work is refunded. */
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_execute(XrCliToolchainUpdate *);
XR_FUNC const XrCliToolchainUpdateView *xr_cli_toolchain_update_view(const XrCliToolchainUpdate *);
/* No new allocation/work. PENDING retains owner, lock and publication cleanup
 * for retry; OK frees/nulls. A NULL handle is already closed. */
XR_FUNC XrCliToolchainUpdateStatus xr_cli_toolchain_update_close(XrCliToolchainUpdate **);
#endif // XCLI_TOOLCHAIN_UPDATE_H
