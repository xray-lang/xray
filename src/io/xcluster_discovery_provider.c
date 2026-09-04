/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_discovery_provider.c - Multicast socket projection for cluster.xr
 *
 * The Xray module owns the wire format, cadence, admission, routing and
 * lifecycle. This leaf only translates one multicast socket request into an
 * opaque NetConn so the normal net.xr datagram and netpoll paths can drive it.
 */

#include "xcluster_discovery_provider.h"
#include "xnet_handle.h"
#include "../os/os_net.h"
#include "../os/os_thread.h"
#include "../runtime/object/xstring.h"

#include <string.h>

#ifdef XR_OS_WINDOWS
static xr_once_t g_discovery_winsock_once = XR_ONCE_INITIALIZER;

static void discovery_winsock_init_once(void) {
    (void) xr_winsock_init();
}

static void discovery_platform_init(void) {
    xr_once_call(&g_discovery_winsock_once, discovery_winsock_init_once);
}
#else
static void discovery_platform_init(void) {
}
#endif

static XrValue discovery_socket_fail(xr_socket_t fd) {
    if (fd != XR_INVALID_SOCKET)
        xr_closesocket(fd);
    return XR_NULL_VAL;
}

XrValue xr_cluster_discovery_socket_open(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 4 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]) ||
        !XR_IS_BOOL(args[3]))
        return XR_NULL_VAL;

    const char *group = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    int port = (int) XR_TO_INT(args[1]);
    int ttl_value = (int) XR_TO_INT(args[2]);
    bool loopback = XR_TO_BOOL(args[3]) != 0;
    if (port <= 0 || port > 65535 || ttl_value < 0 || ttl_value > 255)
        return XR_NULL_VAL;

    struct in_addr group_address;
    if (inet_pton(AF_INET, group, &group_address) != 1)
        return XR_NULL_VAL;
    discovery_platform_init();
    xr_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == XR_INVALID_SOCKET)
        return XR_NULL_VAL;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t) port);
    struct ip_mreq membership;
    membership.imr_multiaddr = group_address;
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    unsigned char ttl = (unsigned char) ttl_value;
    unsigned char loop = loopback ? 1u : 0u;

    if (xr_socket_set_reuseaddr(fd, true) != 0 || xr_socket_set_reuseport(fd, true) != 0 ||
        bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &membership,
                   sizeof(membership)) != 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop)) != 0 ||
        xr_socket_set_nonblocking(fd) != 0)
        return discovery_socket_fail(fd);

    XrNetConn *handle = xr_net_conn_new(X, (int) fd, XR_NETCONN_UDP);
    if (!handle)
        return discovery_socket_fail(fd);
    return XR_FROM_PTR(handle);
}
