#!/bin/bash
set -u

XRAY="$1"
FIXTURE="$2"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_native_output.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

cp -R "$FIXTURE" "$WORK/project"

if ! (cd "$WORK/project" && "$XRAY" build --native --cache-dir "$WORK/cache" \
        -o "$WORK/native-output" main.xr >"$WORK/build.log" 2>&1); then
    sed -n '1,160p' "$WORK/build.log" >&2
    exit 1
fi

if [ "$("$WORK/native-output")" != "42" ]; then
    echo "native output parameter did not materialize CValue{42}" >&2
    exit 1
fi

if (cd "$WORK/project" && "$XRAY" run main.xr >"$WORK/vm.log" 2>&1); then
    echo "VM unexpectedly accepted a package declared vm=unsupported" >&2
    exit 1
fi
grep -Fq "does not support the VM backend" "$WORK/vm.log" || {
    sed -n '1,120p' "$WORK/vm.log" >&2
    exit 1
}

cp -R "$FIXTURE" "$WORK/bad-layout"
perl -pi -e 's/int64_t value/int32_t value/' "$WORK/bad-layout/native_output.h"
if (cd "$WORK/bad-layout" && "$XRAY" build --native --cache-dir "$WORK/bad-cache" \
        -o "$WORK/bad-output" main.xr >"$WORK/bad-layout.log" 2>&1); then
    echo "mismatched C header layout unexpectedly passed" >&2
    exit 1
fi
grep -Fq "E-NATIVE-LAYOUT" "$WORK/bad-layout.log" || {
    sed -n '1,160p' "$WORK/bad-layout.log" >&2
    exit 1
}

echo "native output boundary tests passed"
