/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XMODULE_OVERLAY_H
#define XMODULE_OVERLAY_H
#include "xmodule_identity.h"

/* Borrowed input for one immutable compiler-owned overlay set. Text length
 * excludes a terminator. No file must exist; no OS alias equivalence is added. */
typedef struct XrModuleOverlayInput {
    XrModuleIdentityAuthority authority;
    const char *logical_path, *source_path, *text;
    size_t length;
} XrModuleOverlayInput;
typedef struct XrModuleOverlayEntry {
    const char *canonical;
    XrModuleOverlayInput source;
} XrModuleOverlayEntry;
typedef struct XrModuleOverlay XrModuleOverlay;

/* Deep copies every coordinate and exactly length text bytes. Duplicate
 * identities under the same physical authority are rejected. The same portable
 * identity under different physical roots is independent. A physical document
 * may have several authority bindings only with exactly identical text.
 * Embedded NUL is invalid. Failure preserves output; an occupied output is
 * invalid. Rooted source paths must derive the supplied logical identity.
 * MEMORY accepts no source path and only an empty or NULL logical path. */
XR_FUNC XrModuleStatus xr_compile_module_overlay_new(XrCompileResources *resources,
    const XrModuleOverlayInput *inputs, size_t count, XrModuleOverlay **output);
XR_FUNC void xr_compile_module_overlay_free(XrModuleOverlay *overlay);
XR_FUNC XrCompileResources *xr_compile_module_overlay_resources(const XrModuleOverlay *overlay);
/* Entries and their bytes are immutable borrows ending at overlay_free. */
XR_FUNC const XrModuleOverlayEntry *xr_compile_module_overlay_entries(const XrModuleOverlay *overlay, size_t *count);
/* Missing canonical identity publishes NULL on OK. A matching identity with
 * another physical root or authority is INVALID, never a silent miss. Failed
 * work or validation preserves output. Hash/probe and equality are metered. */
XR_FUNC XrModuleStatus xr_compile_module_overlay_lookup(const XrModuleOverlay *overlay,
    const char *canonical, const XrModuleIdentityAuthority *authority,
    const XrModuleOverlayEntry **output);
/* Bind text by the exact physical document after deriving and validating its
 * identity under the importing authority. The returned entry supplies text
 * only: its original declaration authority is never substituted for authority.
 * Only slash spelling is normalized on Windows; no OS alias equivalence.
 * Caller must independently reject immutable Catalog collisions. */
XR_FUNC XrModuleStatus xr_compile_module_overlay_bind(const XrModuleOverlay *,
    const XrModuleIdentityAuthority *,const char *source_path,const XrModuleOverlayEntry **);
#endif
