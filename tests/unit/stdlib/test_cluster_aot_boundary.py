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


if __name__ == "__main__":
    unittest.main()
