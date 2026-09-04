#!/usr/bin/env python3
"""Keep cluster's native backend on the source-owned runtime boundary."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import parse_defs  # noqa: E402


SOURCE_OWNED_LEAVES = {
    "__start",
    "__stop",
    "__broadcast",
}

RETIRED_STANDALONE_FILES = (
    "src/aot/xrt_cluster.c",
    "src/aot/xrt_cluster.h",
    "src/coro/xcluster_blocking_runtime.c",
    "src/coro/xcluster_blocking_runtime.h",
    "src/coro/xphi_detector.c",
    "src/coro/xphi_detector.h",
    "src/coro/xtopic_registry.c",
    "src/coro/xtopic_registry.h",
    "src/io/xcluster_blocking.c",
    "src/io/xcluster_blocking.h",
)


class ClusterAotBoundaryTests(unittest.TestCase):
    def test_source_owned_leaves_have_no_parallel_aot_dispatch(self) -> None:
        entries = {
            entry.name: entry
            for entry in parse_defs(ROOT)
            if entry.module == "cluster" and entry.name in SOURCE_OWNED_LEAVES
        }
        self.assertEqual(SOURCE_OWNED_LEAVES, set(entries))
        for name, entry in entries.items():
            self.assertEqual("", entry.aot, name)
            self.assertFalse(entry.aot_direct, name)
            self.assertEqual("", entry.link_object, name)

    def test_standalone_cluster_runtime_is_absent(self) -> None:
        for relative in RETIRED_STANDALONE_FILES:
            self.assertFalse((ROOT / relative).exists(), relative)
        self.assertNotIn('include "xrt_cluster.h"', (ROOT / "src/aot/xrt.h").read_text())

    def test_generated_aot_tables_have_no_cluster_dispatch(self) -> None:
        for relative in (
            "src/aot/xstdlib_aot_methods_generated.inc.c",
            "src/aot/xaot_stdlib_generated.inc.c",
        ):
            self.assertNotIn("xrt_cluster_", (ROOT / relative).read_text(), relative)

    def test_phi_state_and_heartbeat_admission_are_source_owned(self) -> None:
        cluster_c = (ROOT / "stdlib/cluster/cluster.c").read_text()
        cluster_h = (ROOT / "stdlib/cluster/cluster_internal.h").read_text()
        cluster_xr = (ROOT / "stdlib/cluster/cluster.xr").read_text()
        health_snapshot = cluster_c.split(
            "static XrValue cluster_health_snapshot_fn", 1
        )[1].split("static XrValue cluster_health_apply_fn", 1)[0]

        self.assertNotIn("XrPhiDetector", cluster_h)
        self.assertNotIn("last_heartbeat_sent", cluster_h)
        self.assertNotIn("xr_cluster_output_queue", health_snapshot)
        self.assertIn("var _phiPeerStates: Array<_PhiPeerState>", cluster_xr)
        self.assertIn("var peers = __healthSnapshot()", cluster_xr)
        self.assertIn(
            "_enqueueControlFrame(peer.peerGeneration, heartbeat!)", cluster_xr
        )


if __name__ == "__main__":
    unittest.main()
