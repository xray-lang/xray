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
#include "../xir/xxir_ctfe.h"

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
    XrXirSourceDiagnosticFact source;
    XrXirDiagnostic xir;
    XrXirSourceSnapshot *snapshot;
    XrXirSourceFailure failure;
} XrXirSourceProductDiagnostic;
/* Failure preserves output. Diagnostics own their semantic failure path and
 * any complete query snapshot retained after a later projection failure. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_build(const XrXirSourceProductRequest *request,
    XrXirSourceProduct **output,XrXirSourceProductDiagnostic *diagnostic);
/* Explicit text has exactly the source_check_text authority/path contract.
 * The buffer and all request fields are borrowed synchronously. No file is
 * created or read for the entry. Successful products own the same Checked,
 * specialization, Lowered, queries and transferable Program as file products.
 * Failure preserves output and owns any published diagnostic path/snapshot. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_build_text(const XrXirSourceProductRequest *request,
    const XrXirSourceText *text, XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic);
/* The actual owning AST is borrowed with source_check_ast's lifetime and
 * authority contract. The same Checked, specialization, Lowered and genuine
 * transferable Program pipeline owns the result independently of that AST. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_build_ast(const XrXirSourceProductRequest *request,
    const XrXirSourceAst *ast, XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_source_product_free(XrXirSourceProduct *product);
XR_FUNC void xr_xir_compile_source_product_diagnostic_free(XrXirSourceProductDiagnostic *diagnostic);
/* Public execution admission reads the owner's authentic Program root and all
 * state slots on the original ledger. Imported mutable slots are rejected;
 * root mutable state and checked Sendable library constants remain admitted.
 * This does not bypass complete Source/Checked/specialization verification.
 * It requires the Lowered artifact before VM transfer. Diagnostic storage is a
 * plain value and receives the result; no storage is allocated or published. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_public_admit(
    const XrXirSourceProduct *product, XrXirSourceDiagnostic *diagnostic);
/* Applies the identical public state policy to an actual owned Lowered image.
 * The image supplies its real context and declaration authority. No caller
 * flags, names or query facts can grant admission. */
XR_FUNC XrXirStatus xr_xir_compile_source_lowered_public_admit(
    const XrXirArtifact *lowered, XrXirSourceDiagnostic *diagnostic);
/* Borrowed immutable facts and packets remain valid after VM code transfer. */
XR_FUNC const XrXirSourceProductFacts *xr_xir_compile_source_product_facts(const XrXirSourceProduct *product);
/* The borrowed context is the owner's actual ledger, including after VM take. */
XR_FUNC const XrXirCompileContext *xr_xir_compile_source_product_context(const XrXirSourceProduct *product);
/* Borrows the authentic immutable Lowered module consumed by VM preparation.
 * Valid only before successful VM take and while the product stays alive.
 * This is a typed owned view, not another serialized receive or validation. */
XR_FUNC const XrXirModule *xr_xir_compile_source_product_module(const XrXirSourceProduct *product);

/* Root-module roles use actual closed function indexes. Entries and names are
 * product-owned on its original ledger, survive VM transfer and producer
 * destruction, and are borrowed until product free. They confer no authority. */
XR_FUNC const XrXirSourceTests *xr_xir_compile_source_product_tests(const XrXirSourceProduct *product);
XR_FUNC XrXirStatus xr_xir_compile_source_product_packet(const XrXirSourceProduct *product,
    XrXirSourceProductPacketKind kind,XrXirSourceProductPacketView *output);
XR_FUNC const XrXirSourceView *xr_xir_compile_source_product_view(const XrXirSourceProduct *product);
/* Closed function indexes and instructions are projected through authentic
 * specialization origins. Points remain product-owned after VM transfer. */
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_product_debug(const XrXirSourceProduct *product);
XR_FUNC const XrXirConstruction *xr_xir_compile_source_product_construction(const XrXirSourceProduct *product);
/* Layout borrows end when the Lowered artifact transfers to a VM program. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_layout(const XrXirSourceProduct *product,
    uint32_t function,XrXirSourceProductLayoutView *output);
/* Successful VM projection transfers code ownership and leaves queries owned. */
XR_FUNC XrXirStatus xr_xir_compile_source_product_vm_take(XrXirSourceProduct *product,
    XrXirProgram **output);
/* Before VM transfer only. The owner's exact target and closed packet digest
 * feed ordinary scalar CTFE admission on its original controlled memory owner. Control is borrowed only during this
 * synchronous call and is never retained in the product or result.
 * This grants no runtime, root, native or mutation authority. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_source_product_scalar(const XrXirSourceProduct *,uint32_t,
    const XrXirValue *,uint32_t,XrXirCtfeLimits,XrCompileRequestControl *,XrXirValue *);
XR_FUNC XrXirStatus xr_xir_compile_source_product_emit(const XrXirSourceProduct *product,
    const char *prefix,size_t byte_limit,XrXirCSource *output);
/* Same-source dependency observation remains owned after executable transfer. */
XR_FUNC const XrXirSourceDependencies *xr_xir_compile_source_product_dependencies(
    const XrXirSourceProduct *product);
#endif // XR_XIR_SOURCE_PRODUCT_H
