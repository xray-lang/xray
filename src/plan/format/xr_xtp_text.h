/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_text.h - Deterministic textual rendering and comparison of artifacts
 *
 * KEY CONCEPT:
 *   The rendering is a pure function of the artifact bytes. It carries no
 *   address, no clock reading, and no allocation identity, so the same
 *   artifact always renders byte for byte the same text and two renderings
 *   may be compared directly. Comparison itself reads the wire rows rather
 *   than the text, so a difference is reported at its exact table row even
 *   when the two rows render alike.
 */

#ifndef XR_XTP_TEXT_H
#define XR_XTP_TEXT_H

#include "xr_xtp_schema.h"
#include <stdio.h>

/* Rendering revision. It changes whenever the emitted text changes shape, so
 * a stored rendering states the reader it was produced for. */
#define XR_XTP_TEXT_REVISION UINT32_C(1)

/* Largest number of neighbouring rows a comparison may print on each side of
 * the first differing row. */
#define XR_XTP_TEXT_MAX_CONTEXT_ROWS UINT32_C(64)

/* Write the complete rendering of one decoded candidate. Every identity
 * field, every resource bound, every section header, and every table row is
 * emitted in wire order. */
XR_FUNC bool xr_xtp_candidate_dump(const XrXtpCandidate *candidate, FILE *out);

/* Compare two decoded candidates and report the first difference, with up to
 * context_rows neighbouring rows when the difference is a table row.
 * Reports nothing when the two artifacts are identical. Returns false only
 * when the comparison could not be performed at all; a difference is a
 * successful comparison and is reported through identical. */
XR_FUNC bool xr_xtp_candidate_diff(const XrXtpCandidate *left,
                                   const XrXtpCandidate *right,
                                   uint32_t context_rows, FILE *out,
                                   bool *identical);

#endif  // XR_XTP_TEXT_H
