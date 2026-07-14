#!/usr/bin/env python3
"""ParamMode display coverage for generated tooling surfaces."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_mcp_knowledge as mcp_knowledge  # noqa: E402
import gen_stdlib_types as stdlib_types  # noqa: E402


PARAM_MODE_SIGNATURE = "(view: in Slice<byte>, sink: ref Array<byte>, outLen: out int): int"


class ParamModeToolingDisplayTest(unittest.TestCase):
    def test_lsp_signature_display_preserves_param_modes(self) -> None:
        self.assertEqual(
            f"fn{PARAM_MODE_SIGNATURE}",
            stdlib_types.lsp_signature(PARAM_MODE_SIGNATURE),
        )

        lsp_include = stdlib_types.generate_lsp_include({
            "modes": {
                "handles": [],
                "constants": [],
                "methods": [
                    {
                        "name": "borrow",
                        "signature": PARAM_MODE_SIGNATURE,
                        "doc": "Borrow and fill output.",
                    }
                ],
            }
        })

        self.assertIn(
            f'"fn{PARAM_MODE_SIGNATURE}"',
            lsp_include,
        )

    def test_xrd_generation_preserves_param_modes(self) -> None:
        xrd = stdlib_types.generate_xrd({
            "module": "modes",
            "handles": [],
            "constants": [],
            "methods": [
                {
                    "name": "borrow",
                    "signature": PARAM_MODE_SIGNATURE,
                    "doc": "Borrow and fill output.",
                }
            ],
            "handle_methods": {
                "ModeHandle": [
                    {
                        "name": "fill",
                        "signature": PARAM_MODE_SIGNATURE,
                        "doc": "Fill through a handle.",
                    }
                ],
            },
        })

        self.assertIn(
            f"export fn borrow{PARAM_MODE_SIGNATURE}",
            xrd,
        )
        self.assertIn(
            f"fn ModeHandle.fill{PARAM_MODE_SIGNATURE}",
            xrd,
        )

    def test_mcp_knowledge_api_table_preserves_param_modes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-param-mode-tooling.") as tmp:
            inventory_path = Path(tmp) / "api_inventory.json"
            inventory_path.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "items": [
                            {
                                "category": "stdlib",
                                "namespace": "modes",
                                "name": "borrow",
                                "qualified": "modes.borrow",
                                "kind": "function",
                                "signature": PARAM_MODE_SIGNATURE,
                                "summary": "Borrow and fill output.",
                                "source": "stdlib/modes/modes.xr",
                                "line": 1,
                                "doc_surface": "stdlib",
                                "doc_module": "modes",
                            }
                        ],
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )

            symbols = mcp_knowledge.load_api_inventory(inventory_path)["modes"]

        body = mcp_knowledge.render_stdlib_body(
            "modes",
            "Mode helpers.\n",
            symbols,
        )

        self.assertIn(
            f"| `modes.borrow` | `{PARAM_MODE_SIGNATURE}` | Borrow and fill output. |",
            body,
        )


if __name__ == "__main__":
    unittest.main()
