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

if [ -e "$WORK/project/native_output.o" ]; then
    echo "native unit leaked a target-specific object into the package tree" >&2
    exit 1
fi

(
    cd "$WORK/project" &&
        "$XRAY" build --native --target x86_64-linux-musl --cache-dir "$WORK/cross-cache" \
            -o "$WORK/native-output-linux" main.xr >"$WORK/cross-linux.log" 2>&1
) &
linux_pid=$!
(
    cd "$WORK/project" &&
        "$XRAY" build --native --target x86_64-windows-gnu --cache-dir "$WORK/cross-cache" \
            -o "$WORK/native-output-windows.exe" main.xr >"$WORK/cross-windows.log" 2>&1
) &
windows_pid=$!
cross_failed=0
wait "$linux_pid" || cross_failed=1
wait "$windows_pid" || cross_failed=1
if [ "$cross_failed" -ne 0 ]; then
    sed -n '1,160p' "$WORK/cross-linux.log" >&2
    sed -n '1,160p' "$WORK/cross-windows.log" >&2
    exit 1
fi
if [ ! -s "$WORK/native-output-linux" ] || [ ! -s "$WORK/native-output-windows.exe" ]; then
    echo "parallel cross-target native outputs were not produced" >&2
    exit 1
fi
if [ -e "$WORK/project/native_output.o" ]; then
    echo "parallel cross-target native units shared a package-tree object" >&2
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
