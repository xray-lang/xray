/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_enum_storage.h - Explicit policy owned enum metadata
 */
#ifndef XANALYZER_ENUM_STORAGE_H
#define XANALYZER_ENUM_STORAGE_H
#include "xanalyzer_symbol.h"
#include "../../base/xio_policy.h"

/* Compiler producers pass their original ledger policy and check their original
 * CompileState before borrowed inputs. Display-only callers choose an explicit
 * system policy. Payload types remain borrowed from their actual graph owners. */
/* Prepare metadata under caller exclusion and publish it after finalization.
 * Successful replacement invalidates the replaced owned buffers; failures
 * preserve all previously published fields. */
XR_FUNC XrOsIoStatus xa_enum_info_new(const XrOsIoPolicy *policy, const char *nominal_owner,
                                      const char *name, uint32_t count, XaEnumInfo **output);
XR_FUNC XrOsIoStatus xa_enum_variant_set_name(XaEnumVariantInfo *variant, const char *name);
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_names(XaEnumVariantInfo *variant,
                                                       const char *const *names, uint16_t count);
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_locations(XaEnumVariantInfo *variant,
                                                           const char *file,
                                                           const XrNameSpan *spans, uint16_t count);
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_types(XaEnumVariantInfo *variant,
                                                       XrType *const *types, uint16_t count);
XR_FUNC XrOsIoStatus xa_enum_info_finalize_layout(XaEnumInfo *info);
XR_FUNC XrOsIoStatus xa_enum_info_clone(const XrOsIoPolicy *policy, const XaEnumInfo *source,
                                        XaEnumInfo **output);
XR_FUNC XrCompileResourceStatus xa_enum_capture_status(XrCompileState *state, XrOsIoStatus status);
#endif  // XANALYZER_ENUM_STORAGE_H
