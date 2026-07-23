/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_json.h - Stable machine-readable toolchain protocol v1
 */

#ifndef XTC_JSON_H
#define XTC_JSON_H

#include "xtc_probe.h"

#include <stdio.h>

XR_FUNC void xtc_json_write_string(FILE *out, const char *text);
XR_FUNC bool xtc_probe_json_write(FILE *out, const char *requested_target,
                                  const XrToolchainProbeOptions *options,
                                  const XrToolchainProbeResult *result, bool ready,
                                  const char *xray_version, const char *xray_build);

#endif /* XTC_JSON_H */
