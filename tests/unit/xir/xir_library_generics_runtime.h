/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_generics_runtime.h - Owned execution bridge
 */
#ifndef XIR_LIBRARY_GENERICS_RUNTIME_H
#define XIR_LIBRARY_GENERICS_RUNTIME_H
XR_FUNC void library_generics_execute_owned(XrXirArtifact *owned,const char *prefix,const char *c_path);
#endif // XIR_LIBRARY_GENERICS_RUNTIME_H
