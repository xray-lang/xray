/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_fingerprint.h - One domain-separated source identity algorithm
 */
#ifndef XMODULE_FINGERPRINT_H
#define XMODULE_FINGERPRINT_H
#include "../base/xdefs.h"
#include "../base/xstable_id.h"

/* Source identity is domain- and length-framed; a plain content hash is not
 * interchangeable. This pure query maps NULL to the empty-source identity. */
XR_FUNC void xr_module_source_fingerprint(const char *source, XrFingerprint *output);
#endif /* XMODULE_FINGERPRINT_H */
