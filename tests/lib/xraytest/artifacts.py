"""Publish eligibility for native test artifact retention after complete success."""
from pathlib import Path
import json
import os
import tempfile


def complete_native_artifacts(root):
    """Call only after every suite assertion; intermediate results are not terminal.

    The marker permits a separate maintenance process to reclaim old generated
    binaries. Source inputs, commands, observations and result files remain.
    Tests use unique attempt directories and never rewrite a completed attempt.
    """
    root = Path(root)
    marker = root / 'cleanup-complete.json'
    if marker.exists():
        raise FileExistsError(marker)
    descriptor, temporary = tempfile.mkstemp(prefix='.cleanup-complete-', dir=root)
    temporary = Path(temporary)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write(json.dumps({'state': 'PASS', 'schema': 1,
                                     'kind': 'native-test-products'}) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, marker)
    finally:
        temporary.unlink(missing_ok=True)
