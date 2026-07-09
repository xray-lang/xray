#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 /path/to/xray" >&2
  exit 2
fi

XRAY="$1"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

"$XRAY" test "$ROOT/tests/vm/http_static_route_full_parse.xr"
