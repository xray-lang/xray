/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dylib_data.c - A real loader-owned data export with no function ABI
 *
 * KEY CONCEPT:
 *   Tests resolve actual readonly DLL storage, never a fabricated address or
 *   a function pointer cast to data. The executable does not link this DLL.
 */
#include "base/xdefs.h"
#if defined(XR_OS_WINDOWS)
__declspec(dllexport) const unsigned char xray_dylib_bytes[3]={0,255,42};
#else
XRAY_API const unsigned char xray_dylib_bytes[3]={0,255,42};
#endif
