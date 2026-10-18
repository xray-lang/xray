from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
import time

exe = Path(sys.argv[1]).resolve()
image = hashlib.sha256(exe.read_bytes()).hexdigest()
body = b'{"jsonrpc":"2.0","method":"probe/notice"}'
frame = b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body
started = time.monotonic()
deadline = started + 25

def remaining():
    value = deadline - time.monotonic()
    if value <= 0:
        raise TimeoutError("original 25-second frame child deadline expired")
    return value

with subprocess.Popen([str(exe), str((1 << 64) - 1), "1", "--wait-input"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as child:
    reader = ThreadPoolExecutor(max_workers=1)
    try:
        waiting = reader.submit(child.stderr.readline).result(timeout=remaining())
        assert waiting.rstrip(b"\r\n") == b"FRAME_WAIT", waiting
        # Input starts only after the actual nonblocking transport reports waiting.
        output, error = child.communicate(frame + frame, timeout=remaining())
        assert child.returncode == 0, (child.returncode, output, waiting + error)
    finally:
        try:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=remaining())
        finally:
            reader.shutdown(wait=True)
assert output == b"", output
assert b"AddressSanitizer" not in error and b"runtime error:" not in error
assert re.search(rb"FRAME scenario=1 frames=2 dispatches=2 sites=[0-9]+ injected=0 exit=0 JSON_and_transport0/0 dispatch_spy=1", error), error
assert hashlib.sha256(exe.read_bytes()).hexdigest() == image
print(json.dumps(dict(waiting_observed=True, payload_sent_after_wait=True, frames=2, dispatches=2,
    physical_live=0, physical_bytes=0, image_sha256=image, elapsed=time.monotonic()-started,
    production_server=False)))
