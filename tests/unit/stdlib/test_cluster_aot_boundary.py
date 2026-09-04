#!/usr/bin/env python3
"""Keep cluster source-only and its transport on general runtime boundaries."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import parse_defs  # noqa: E402


RETIRED_NATIVE_LEAVES = (
    "__start",
    "__stop",
)

RETIRED_NATIVE_FILES = (
    "src/aot/xrt_cluster.c",
    "src/aot/xrt_cluster.h",
    "src/coro/xcluster_blocking_runtime.c",
    "src/coro/xcluster_blocking_runtime.h",
    "src/coro/xcluster_output_queue.c",
    "src/coro/xcluster_output_queue.h",
    "src/coro/xphi_detector.c",
    "src/coro/xphi_detector.h",
    "src/coro/xtopic_registry.c",
    "src/coro/xtopic_registry.h",
    "src/io/xcluster_blocking.c",
    "src/io/xcluster_blocking.h",
    "src/io/xcluster_peer_transport.c",
    "src/io/xcluster_peer_transport.h",
    "stdlib/cluster/cluster.c",
    "stdlib/cluster/cluster.h",
    "stdlib/cluster/cluster_internal.h",
    "stdlib/cluster/cluster_node.c",
)

RETIRED_TRANSPORT_FILES = (
    "src/io/xnet_transport.c",
    "src/io/xnet_transport.h",
)

RETIRED_TRANSPORT_IDENTITIES = (
    "XrIOConn",
    "xr_io_connect",
    "xr_io_connect_tls_with_ctx",
    "xr_io_close",
    "xr_io_listen",
    "xr_io_conn_from_fd",
    "xr_io_conn_take_net_handle",
    "xr_io_conn_read_try",
    "xr_io_conn_write_try",
    "xr_io_set_timeout",
    "xr_io_set_nonblocking",
    "xnet_transport",
)


class ClusterAotBoundaryTests(unittest.TestCase):
    def test_cluster_has_no_native_definition_module(self) -> None:
        cluster_entries = [entry.name for entry in parse_defs(ROOT) if entry.module == "cluster"]
        self.assertEqual([], cluster_entries)

        core_def = (ROOT / "stdlib/defs/core.def").read_text()
        self.assertNotIn("module cluster {", core_def)
        for leaf in RETIRED_NATIVE_LEAVES:
            self.assertNotIn(f"fn {leaf} ", core_def, leaf)

    def test_native_cluster_runtime_is_absent(self) -> None:
        present = [relative for relative in RETIRED_NATIVE_FILES if (ROOT / relative).exists()]
        self.assertEqual([], present)
        self.assertNotIn('include "xrt_cluster.h"', (ROOT / "src/aot/xrt.h").read_text())

    def test_retired_transport_facade_is_absent(self) -> None:
        present = [relative for relative in RETIRED_TRANSPORT_FILES if (ROOT / relative).exists()]
        self.assertEqual([], present)

        sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src").rglob("*"))
            if path.suffix in {".c", ".h"}
        )
        for identity in RETIRED_TRANSPORT_IDENTITIES:
            self.assertNotIn(identity, sources, identity)

        provider = (ROOT / "src/io/xnet_provider.c").read_text(encoding="utf-8")
        self.assertIn("xr_socket_listen(", provider)
        self.assertIn("xr_socket_set_nonblocking(", provider)

    def test_build_has_no_cluster_c_exception(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text()

        self.assertNotIn("stdlib/cluster/.*\\\\.c", cmake)

    def test_generated_aot_tables_have_no_cluster_dispatch(self) -> None:
        for relative in (
            "src/aot/xstdlib_aot_methods_generated.inc.c",
            "src/aot/xaot_stdlib_generated.inc.c",
        ):
            self.assertNotIn("xrt_cluster_", (ROOT / relative).read_text(), relative)

    def test_peer_transport_and_health_are_source_owned(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()

        self.assertIn("var _phiPeerStates: Array<_PhiPeerState>", cluster_xr)
        self.assertIn("var _peerTransports: Array<_PeerTransportState>", cluster_xr)
        self.assertIn("committed: bool", cluster_xr)
        self.assertIn("endpoint!.ready.trySend(1)", cluster_xr)
        self.assertIn("fn _readPeerFrame(runtimeToken: i64, peerGeneration: i64,", cluster_xr)
        self.assertIn("peer.queuedBytes >= OUTPUT_QUEUE_HIGH_WATERMARK_BYTES", cluster_xr)
        self.assertIn("Coro.yield()", cluster_xr)
        self.assertIn("kind == FRAME_CORO_EXIT", cluster_xr)
        self.assertIn("fn _advanceMissedHeartbeat", cluster_xr)
        self.assertIn("var peers = _peerTransportHealthSnapshots(token)", cluster_xr)
        self.assertIn(
            "_enqueueControlFrame(peer.peerGeneration, heartbeat!)", cluster_xr
        )

    def test_lifecycle_has_no_native_leaf(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()

        referenced = [leaf for leaf in RETIRED_NATIVE_LEAVES if f"{leaf}(" in cluster_xr]
        self.assertEqual([], referenced)

    def test_tls_context_policy_is_source_owned(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()

        self.assertIn("var _sourceTlsResources: _ClusterTlsResources?", cluster_xr)
        self.assertIn("context!.upgrade", cluster_xr)
        self.assertIn("context!.accept", cluster_xr)

    def test_tls_provider_is_a_general_net_boundary(self) -> None:
        net_xr = (ROOT / "stdlib/net/net.xr").read_text()
        tls_provider = (ROOT / "src/io/xtls_provider.c").read_text()
        tls_conn = tls_provider.split("struct XrTlsConn", 1)[1].split("};", 1)[0]

        self.assertIn("export final class TlsClientContext", net_xr)
        self.assertIn("export final class TlsServerContext", net_xr)
        self.assertIn("requireClientCertificate: bool", net_xr)
        self.assertNotIn("XrTlsContext *ctx", tls_conn)
        self.assertIn("SSL_VERIFY_FAIL_IF_NO_PEER_CERT", tls_provider)

    def test_multicast_provider_is_a_general_net_boundary(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()
        net_xr = (ROOT / "stdlib/net/net.xr").read_text()
        core_def = (ROOT / "stdlib/defs/core.def").read_text()
        net_provider = (ROOT / "src/io/xnet_provider.c").read_text()

        self.assertIn("net.udpMulticastBind(", cluster_xr)
        self.assertIn("export fn udpMulticastBind(", net_xr)
        self.assertIn("fn __udpMulticastBind {", core_def)
        self.assertIn("net_udp_multicast_bind_handle", net_provider)

    def test_generation_fences_cover_every_long_lived_source_path(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()

        self.assertIn("fn _closeTopicSubscriptions(runtimeToken: i64)", cluster_xr)
        self.assertIn("fn _dispatchInboundFrame(runtimeToken: i64,", cluster_xr)
        self.assertIn("fn _deliverTopicForRuntime(runtimeToken: i64,", cluster_xr)
        self.assertIn("fn _broadcastPeersForRuntime(runtimeToken: i64,", cluster_xr)
        self.assertIn("type _DiscoveryGeneration = {", cluster_xr)
        self.assertIn("socket: NetConn?", cluster_xr)
        self.assertIn("fn _installDiscoverySocket(runtimeToken: i64,", cluster_xr)
        self.assertIn("_stopDiscovery(retiredToken)", cluster_xr)
        self.assertNotIn("const _discoveryActiveToken", cluster_xr)

    def test_public_status_surface_contains_only_observable_facts(self) -> None:
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()

        self.assertNotIn("export enum ClusterNodeState", cluster_xr)
        self.assertNotIn("export type ClusterTlsStatus", cluster_xr)
        self.assertNotIn("running: bool", cluster_xr)
        self.assertIn("endpoint: Endpoint?", cluster_xr)
        self.assertNotIn("export class NodeAddress", cluster_xr)
        self.assertIn("export fn parseAddress(addr: string) -> Endpoint", cluster_xr)
        self.assertIn("Endpoint(host, port), ack!.flags", cluster_xr)
        self.assertIn(
            "_adoptReservedPeer(move conn, token, request!.name, null, request!.flags)",
            cluster_xr,
        )
        self.assertIn("if (port <= 0 || port > 65535)", cluster_xr)
        self.assertIn("if (tlsEnabled && len(certFile) == 0) { return false }", cluster_xr)
        self.assertIn("certFile, keyFile, caFile, !insecure)", cluster_xr)


if __name__ == "__main__":
    unittest.main()
