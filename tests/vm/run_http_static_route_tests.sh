#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 /path/to/xray" >&2
  exit 2
fi

XRAY="$1"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

"$XRAY" test "$ROOT/tests/vm/http_static_route_full_parse.xr"

cache_dir="$(mktemp -d "${TMPDIR:-/tmp}/xray-http-static-route-aot.XXXXXX")"
trap 'rm -rf "$cache_dir"' EXIT

"$XRAY" build --native \
  --cache-dir "$cache_dir/cache" \
  -o "$cache_dir/http_static_route_full_parse" \
  "$ROOT/tests/vm/http_static_route_full_parse_main.xr"
"$cache_dir/http_static_route_full_parse"
