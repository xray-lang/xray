/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_udp_retry.c - UDP nonblocking attempts and synchronous driver
 */
#include "aot/xrt_coll.h"
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

int main(void) {
    REQUIRE(xr_winsock_init() == 0);
    xr_socket_t receiver = socket(AF_INET, SOCK_DGRAM, 0);
    xr_socket_t sender = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(receiver != XR_INVALID_SOCKET && sender != XR_INVALID_SOCKET);
    REQUIRE(xr_socket_set_nonblocking(receiver) == 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(receiver, (struct sockaddr *) &address, sizeof(address)) == 0);
    socklen_t address_length = sizeof(address);
    REQUIRE(getsockname(receiver, (struct sockaddr *) &address, &address_length) == 0);
    xrt_net_conn_object_t connection = {0};
    connection.base.handle_kind = XRT_NET_HANDLE_CONN;
    connection.base.fd = receiver;
    connection.conn_kind = XRT_NETCONN_UDP;
    XrValue handle = xrt_net_conn_box(&connection);
    uint8_t bytes[8] = {0};
    xrt_net_array_view_t array = {0};
    array.data = bytes;
    array.capacity = sizeof(bytes);
    array.elem_size = 1;
    array.elem_type = XR_ELEM_U8;
    XrValue buffer = xr_mkptr(&array, XR_TAG_ARRAY);
    XrValue deadline = XR_FROM_INT(xrt_net_now_ms() + 2000);
    xrt_net_try_result_t attempt = xrt_net_udp_recv_into_try(handle, buffer, deadline);
    REQUIRE(attempt.state == XRT_NET_TRY_WAIT_READ && attempt.timeout_ms > 0);
    REQUIRE(array.length == 0 && connection.udp_from_host[0] == 0);
    REQUIRE(sendto(sender, "abc", 3, 0, (struct sockaddr *) &address, sizeof(address)) == 3);
    REQUIRE(xrt_net_wait_fd(receiver, true, XR_TO_INT(deadline)) > 0);
    attempt = xrt_net_udp_recv_into_try(handle, buffer, deadline);
    REQUIRE(attempt.state == XRT_NET_TRY_DONE && XR_TO_INT(attempt.value) == 3);
    REQUIRE(array.length == 3 && memcmp(bytes, "abc", 3) == 0);
    REQUIRE(strcmp(connection.udp_from_host, "127.0.0.1") == 0 && connection.udp_from_port > 0);
    REQUIRE(sendto(sender, "", 0, 0, (struct sockaddr *) &address, sizeof(address)) == 0);
    REQUIRE(XR_TO_INT(xrt_net_udp_recv_into(handle, buffer, deadline)) == 0);
    REQUIRE(array.length == 0);
    XrValue expired = XR_FROM_INT(xrt_net_now_ms() - 1);
    attempt = xrt_net_udp_recv_into_try(handle, buffer, expired);
    REQUIRE(attempt.state == XRT_NET_TRY_WAIT_READ && attempt.timeout_ms == 0);
    REQUIRE(XR_TO_INT(xrt_net_udp_recv_into(handle, buffer, expired)) == -1);
    REQUIRE(connection.base.last_error == XRT_NETERR_TIMEOUT);
    connection.base.closed = true;
    attempt = xrt_net_udp_recv_into_try(handle, buffer, deadline);
    REQUIRE(attempt.state == XRT_NET_TRY_DONE && XR_TO_INT(attempt.value) == -1);
    REQUIRE(connection.base.last_error == XRT_NETERR_CLOSED);
    REQUIRE(xr_closesocket(receiver) == 0 && xr_closesocket(sender) == 0);
    xr_winsock_cleanup();
    puts("UDP retry, payload, empty datagram, timeout and close PASS");
    return 0;
}
