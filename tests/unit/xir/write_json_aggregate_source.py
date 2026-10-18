"""Embed the exact ordinary helper source in an immutable compiler fixture."""
from pathlib import Path
import json
import sys

source, output = map(Path, sys.argv[1:])
raw = source.read_bytes()
text = raw.decode("ascii")
lines = [json.dumps(line) for line in text.splitlines(keepends=True)]
assert lines and "".join(json.loads(line) for line in lines).encode("ascii") == raw
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(
    "/* Exact source bytes are owned by the fixture binary. */\n"
    "static const char aggregate_helper_source[]=\n" + "\n".join(lines) + ";\n",
    encoding="utf-8", newline="\n",
)
