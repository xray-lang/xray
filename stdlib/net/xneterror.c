/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xneterror.c - Unified network error string table
 */

#include "xneterror.h"

static const char *error_strings[] = {
    [XR_NERR_OK] = "Success",
    [XR_NERR_CONNECT] = "Connection failed",
    [XR_NERR_CLOSED] = "Connection closed",
    [XR_NERR_TIMEOUT] = "Timeout",
    [XR_NERR_WOULDBLOCK] = "Would block",
    [XR_NERR_READ] = "Read failed",
    [XR_NERR_WRITE] = "Write failed",
    [XR_NERR_DNS] = "DNS resolution failed",
    [XR_NERR_TLS] = "TLS error",
    [XR_NERR_TLS_INIT] = "TLS initialization failed",
    [XR_NERR_TLS_CERT] = "Certificate error",
    [XR_NERR_TLS_HANDSHAKE] = "TLS handshake failed",
    [XR_NERR_TLS_VERIFY] = "Certificate verification failed",
    [XR_NERR_URL_PARSE] = "URL parse error",
    [XR_NERR_PARSE] = "Protocol parse error",
    [XR_NERR_HANDSHAKE] = "Handshake failed",
    [XR_NERR_PROTOCOL] = "Protocol error",
    [XR_NERR_TOO_LARGE] = "Payload too large",
    [XR_NERR_BIND] = "Bind failed",
    [XR_NERR_LISTEN] = "Listen failed",
    [XR_NERR_ACCEPT] = "Accept failed",
    [XR_NERR_INVALID] = "Invalid argument",
    [XR_NERR_MEMORY] = "Memory allocation failed",
};

const char *xr_net_error_string(XrNetError err) {
    if (err >= 0 && err < XR_NERR__COUNT) {
        return error_strings[err];
    }
    return "Unknown error";
}
