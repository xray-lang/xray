/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_native_provider.h - Host-owned native code for canonical Source receiving
 */
#ifndef XDAP_NATIVE_PROVIDER_H
#define XDAP_NATIVE_PROVIDER_H
#include "../../xir/xxir_vm_debug.h"
#include "../../program/xr_xir_source_product.h"

typedef struct XdapNativeCode {
    XrXirProgramSpec program;
    const XirCallNativeSourceCode *source;
    const unsigned char *mask;
    uint32_t count;
} XdapNativeCode;

/* Called by the sole launch owner after fresh canonical Source/Closed receiving.
 * Source and Lowered inputs borrow only this call and grant no execution rights.
 * Success returns the exact native ProgramSpec plus current emitted descriptor
 * and selection. The code lease pins callbacks and borrowed metadata until the
 * receiving attempt finishes, and pins code until the final target/value dies.
 * Static code may use a null lease. Failure leaves the zero output untouched.
 * Receiving failure releases a successful lease once; successful receiving takes
 * it atomically. No failure falls back to VM or calls this provider a second time.
 * The host keeps provider context alive through launch/restart/adapter disposal;
 * a detached target depends only on the admitted code lease, output and time. */
typedef struct XdapNativeProvider {
    void *context;
    XrXirStatus (*load)(void *,const XrXirSourceProduct *,const XrXirArtifact *,XdapNativeCode *);
} XdapNativeProvider;
#endif // XDAP_NATIVE_PROVIDER_H
