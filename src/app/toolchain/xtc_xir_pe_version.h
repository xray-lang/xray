/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_pe_version.h - Fixed version facts from borrowed PE bytes
 */
#ifndef XTC_XIR_PE_VERSION_H
#define XTC_XIR_PE_VERSION_H
#include "../../base/xcompile_resources.h"

typedef enum XtcXirPeVersionStatus {
    XTC_XIR_PE_VERSION_OK,
    XTC_XIR_PE_VERSION_BAD_ARGUMENT,
    XTC_XIR_PE_VERSION_BUDGET,
    XTC_XIR_PE_VERSION_INVALID,
    XTC_XIR_PE_VERSION_TRUNCATED,
    XTC_XIR_PE_VERSION_UNSUPPORTED,
    XTC_XIR_PE_VERSION_AMBIGUOUS,
    XTC_XIR_PE_VERSION_NOT_FOUND
} XtcXirPeVersionStatus;
typedef struct XtcXirPeVersion {
    uint16_t file[4], product[4];
    char file_text[24], product_text[24];
} XtcXirPeVersion;

typedef struct XtcXirPeImage {
    uint16_t machine, optional_magic, characteristics, subsystem;
    uint32_t entry_rva, size_of_image, size_of_headers;
    uint32_t entry_section_characteristics;
} XtcXirPeImage;

/* Decode bounded AMD64 PE32+ image facts using the same header/RVA reader.
 * The entry must have unique file-backed bytes. Version resources are not
 * required. This does not validate imports or implement the Windows loader.
 * No allocation or I/O occurs; failures preserve the entire output. */
XR_FUNC XtcXirPeVersionStatus xtc_xir_pe_image_parse(XrCompileResources *resources,
    const uint8_t *bytes, size_t length, XtcXirPeImage *output);

/* Parse AMD64 PE32+ fixed version resources without allocation or I/O.
 * All RT_VERSION leaves must have identical fixed information. Strings are
 * not interpreted. Facts neither authenticate the provider nor authorize it.
 * Field reads, fixed-value comparisons, decimal conversion and publication
 * charge the supplied ledger. The caller keeps bytes and ledger alive during
 * the call; bounds checks do not read or charge rejected byte ranges.
 * Output is a self-contained value and is unchanged on every failure. */
XR_FUNC XtcXirPeVersionStatus xtc_xir_pe_version_parse(XrCompileResources *resources,
    const uint8_t *bytes, size_t length, XtcXirPeVersion *output);
#endif // XTC_XIR_PE_VERSION_H
