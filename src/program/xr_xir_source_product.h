/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_source_product.h - One typed source owner for executable projections
 *
 * KEY CONCEPT:
 *   Both VM and native emission consume the same verified Lowered artifact.
 */
#ifndef XR_XIR_SOURCE_PRODUCT_H
#define XR_XIR_SOURCE_PRODUCT_H
#include "../xir/xxir_source_dependencies.h"
#include "../xir/xxir_source.h"
#include "../xir/xxir_checked.h"
#include "../xir/xxir_emit_c.h"
#include "../xir/xxir_program.h"

typedef struct XrXirSourceProduct XrXirSourceProduct;
typedef struct XrXirSourceTestEntry {
    uint32_t function, role, timeout_seconds;
    const char *name;
    size_t name_length;
} XrXirSourceTestEntry;
typedef struct XrXirSourceTests {
    const XrXirSourceTestEntry *entries;
    uint32_t count;
} XrXirSourceTests;
typedef struct XrXirSourceProductFacts {
    XrXirTarget target;
    uint32_t entry, function_count, module_count;
    uint8_t source_digest[32], closed_digest[32], lowered_layout_digest[32];
} XrXirSourceProductFacts;
typedef struct XrXirSourceProductPacketView {
    const uint8_t *bytes;
    size_t length;
} XrXirSourceProductPacketView;
typedef enum XrXirSourceProductPacketKind {
    XR_XIR_SOURCE_PRODUCT_SOURCE, XR_XIR_SOURCE_PRODUCT_CLOSED
} XrXirSourceProductPacketKind;
typedef struct XrXirSourceProductLayoutView {
    uint32_t parameter_count;
    const XrXirFunctionLayout *layout;
} XrXirSourceProductLayoutView;
typedef struct XrXirSourceProductRequest {
    XrXirSourceRequest source;
    XrXirTarget target;
} XrXirSourceProductRequest;
typedef enum XrXirSourceProductStage {
    XR_XIR_SOURCE_PRODUCT_INPUT, XR_XIR_SOURCE_PRODUCT_CHECK,
    XR_XIR_SOURCE_PRODUCT_SPECIALIZE, XR_XIR_SOURCE_PRODUCT_LOWER,
    XR_XIR_SOURCE_PRODUCT_SOURCE_PACKET, XR_XIR_SOURCE_PRODUCT_CLOSED_PACKET,
    XR_XIR_SOURCE_PRODUCT_FACTS
} XrXirSourceProductStage;
typedef struct XrXirSourceProductDiagnostic {
    XrXirSourceProductStage stage;
    XrXirStatus status;
    XrXirSourceDiagnostic source;
    XrXirDiagnostic xir;
    XrXirSourceSnapshot *snapshot;
    char *source_path;
} XrXirSourceProductDiagnostic;
/* Failure preserves output. Diagnostics own their semantic failure path and
 * any complete query snapshot retained after a later projection failure. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_build(const XrXirSourceProductRequest *request,
    XrXirSourceProduct **output,XrXirSourceProductDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_source_product_free(XrXirSourceProduct *product);
XR_FUNC void xr_xir_compile_source_product_diagnostic_free(XrXirSourceProductDiagnostic *diagnostic);
/* Borrowed immutable facts and packets remain valid after VM code transfer. */
XR_FUNC const XrXirSourceProductFacts *xr_xir_compile_source_product_facts(const XrXirSourceProduct *product);
/* The borrowed context is the owner's actual ledger, including after VM take. */
XR_FUNC const XrXirCompileContext *xr_xir_compile_source_product_context(const XrXirSourceProduct *product);
/* Root-module roles use actual closed function indexes. Entries and names are
 * product-owned on its original ledger, survive VM transfer and producer
 * destruction, and are borrowed until product free. They confer no authority. */
XR_FUNC const XrXirSourceTests *xr_xir_compile_source_product_tests(const XrXirSourceProduct *product);
XR_FUNC XrXirStatus xr_xir_compile_source_product_packet(const XrXirSourceProduct *product,
    XrXirSourceProductPacketKind kind,XrXirSourceProductPacketView *output);
XR_FUNC const XrXirSourceView *xr_xir_compile_source_product_view(const XrXirSourceProduct *product);
XR_FUNC const XrXirConstruction *xr_xir_compile_source_product_construction(const XrXirSourceProduct *product);
/* Layout borrows end when the Lowered artifact transfers to a VM program. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_layout(const XrXirSourceProduct *product,
    uint32_t function,XrXirSourceProductLayoutView *output);
/* Rebuild packet correspondence and compare the complete native projection. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_verify(const XrXirSourceProduct *product,
    size_t code_limit,XrXirDiagnostic *diagnostic);
/* Successful VM projection transfers code ownership and leaves queries owned. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_vm_take(XrXirSourceProduct *product,
    XrXirProgram **output);
XR_FUNC XrXirStatus xr_xir_compile_source_product_emit(const XrXirSourceProduct *product,
    const char *prefix,size_t byte_limit,XrXirCSource *output);
/* Same-source dependency observation remains owned after executable transfer. */
XR_FUNC const XrXirSourceDependencies *xr_xir_compile_source_product_dependencies(
    const XrXirSourceProduct *product);
#endif // XR_XIR_SOURCE_PRODUCT_H
