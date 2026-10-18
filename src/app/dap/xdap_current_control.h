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
typedef XrXirCallStatus (*XdapDetachCommit)(void *);
/* Reserve control before committing the host owner. A failed callback leaves
 * observation intact; READY irrevocably transfers host execution. The callback
 * cannot reenter target control. A controlled write must finish first.
 * HOST_ERROR is fatal control I/O: close the session rather than retrying an
 * indeterminate host transition as though it were a precommit rejection. */
XR_FUNC XrXirCallStatus xdap_current_detach(XdapCurrent *,XdapDetachCommit,void *);
#endif // XDAP_CURRENT_CONTROL_H
