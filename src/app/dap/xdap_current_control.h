/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_current_control.h - Retiring observation while preserving target execution
 */
#ifndef XDAP_CURRENT_CONTROL_H
#define XDAP_CURRENT_CONTROL_H
#include "xdap_current.h"
/* Disable observation without cancelling the authenticated Instance. A pending
 * controlled write must finish before ownership can leave the debugger. */
XR_FUNC XrXirCallStatus xdap_current_detach(XdapCurrent *);
#endif // XDAP_CURRENT_CONTROL_H
