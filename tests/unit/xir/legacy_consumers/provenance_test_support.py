"""Locate the actual evidence file for negative mutation controls only."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def provenance_input_path(root: Path, relative: str) -> Path:
    # Use read_text so a test mocking read_bytes can select its target without
    # recursively invoking that same mock. Production validation uses resolve.
    data = json.loads((HERE / 'frozen_derivation_inputs.json').read_text(encoding='utf-8'))
    entry = data['entries'].get(relative)
    return HERE / entry['snapshot'] if entry else root / relative
