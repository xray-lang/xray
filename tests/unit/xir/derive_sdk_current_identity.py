"""Reproduce complete current and original retired SDK known bytes independently."""
from pathlib import Path
import argparse
import hashlib
import sys

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'scripts'))
from derive_xir_sdk_abi import measurements
from sdk_identity_model import current_header, current_manifest, frame, retired_packets, retired_sources

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).parent
    rows = measurements()
    current = current_manifest(rows)
    history = retired_packets(directory)
    image = frame(current)
    current_digest = hashlib.sha256(image).hexdigest()
    assert all(packet['digest_hex'] != current_digest for packet in history)
    outputs = retired_sources(history)
    outputs['sdk_current_identity_golden.h'] = current_header(current)
    for name, text in outputs.items():
        target = directory / name
        data = text.encode('utf-8')
        if args.write:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        else:
            assert target.read_bytes() == data, 'independent complete SDK bytes differ: ' + name
    print(f'Current SDK {len(rows)} fields / {len(image)} byte complete preimage; '
          f'{len(history)} original complete retired packets reproduced: {current_digest}')

if __name__ == '__main__':
    main()
