/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XHIR_DIAGNOSTICS_H
#define XHIR_DIAGNOSTICS_H
#include "../base/xcompile_resources.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The public handle and the compiler report are one allocation graph. */
typedef struct XrSourceDiagnostic XrFrontendReport;
enum { XR_FRONTEND_LOCATION_LINE=1u, XR_FRONTEND_LOCATION_BYTES=2u,
    XR_FRONTEND_LOCATION_UTF16=4u, XR_FRONTEND_LOCATION_REVISION=8u };
typedef struct XrFrontendLocation {
    const char *path;
    uint64_t start_byte,end_byte;
    int32_t line,column,end_line,end_column;
    uint32_t utf16_line,utf16_column,utf16_end_line,utf16_end_column,flags;
    uint8_t revision[32];
} XrFrontendLocation;
typedef struct XrFrontendRoot {
    uint32_t code,severity;
    const char *message;
    XrFrontendLocation primary;
    uint32_t labels;
} XrFrontendRoot;
typedef struct XrFrontendLabel {const char *message;XrFrontendLocation location;} XrFrontendLabel;
typedef struct XrFrontendReportInfo {uint32_t roots;bool complete;} XrFrontendReportInfo;

XR_FUNC XrCompileResourceStatus xr_frontend_report_new(XrCompileResources *,XrFrontendReport **);
XR_FUNC void xr_frontend_report_free(XrFrontendReport *);
/* No source identity or readable bytes are invented for AST-only input. */
XR_FUNC XrCompileResourceStatus xr_frontend_report_source(XrFrontendReport *,const char *,
    const char *,size_t,const uint8_t *,uint32_t *);
XR_FUNC XrFrontendLocation xr_frontend_report_location(const XrFrontendReport *,uint32_t,
    int,int,const char *);
/* Each add publishes its root only after its message and labels are ready. */
XR_FUNC XrCompileResourceStatus xr_frontend_report_add(XrFrontendReport *,uint32_t,
    const char *,XrFrontendLocation,const XrFrontendLabel *,uint32_t);
XR_FUNC void xr_frontend_report_seal(XrFrontendReport *,bool);
XR_FUNC bool xr_frontend_report_info(const XrFrontendReport *,XrFrontendReportInfo *);
XR_FUNC bool xr_frontend_report_root(const XrFrontendReport *,uint32_t,XrFrontendRoot *);
XR_FUNC bool xr_frontend_report_label(const XrFrontendReport *,uint32_t,uint32_t,XrFrontendLabel *);
/* All roots and labels; returned controlled text owns its bytes independently. */
XR_FUNC XrCompileResourceStatus xr_frontend_report_render(const XrFrontendReport *,char **);
#endif
