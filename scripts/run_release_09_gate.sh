#!/usr/bin/env bash
# Local 0.9 consolidation gate.
#
# This gate is intentionally not the final roadmap completion gate. It verifies
# the runnable local baseline after consolidating active code lanes, while
# keeping the broad AOT expectation suites visible as known open boundaries.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${XRAY_RELEASE09_BUILD_DIR:-$PROJECT_DIR/build}"
JOBS="${XRAY_TEST_JOBS:-8}"
CTEST="${CTEST:-ctest}"
DO_BUILD=1
BOUNDARY_REPORT=1
EXTRA_EXCLUDE_RE="${XRAY_RELEASE09_EXTRA_EXCLUDE_RE:-}"

KNOWN_AOT_BOUNDARY_RE='^(aot_filetests|aot_link_command_manifest)$'

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --build-dir DIR          CMake build directory (default: build)
  --jobs N                 Parallel jobs for build/ctest (default: XRAY_TEST_JOBS or 8)
  --ctest PATH             ctest executable (default: ctest)
  --extra-exclude-regex RE  Extra CTest exclusion regex for platform-specific CI debt
  --no-build               Skip cmake --build
  --skip-boundary-report   Do not run known open AOT boundary suites after the gate
  -h, --help               Show this help

The gate excludes only:
  $KNOWN_AOT_BOUNDARY_RE

Those suites currently track long-running AOT file/link expectation work and
are reported separately so a local 0.9 baseline is not blocked on roadmap
completion work.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --ctest)
            CTEST="$2"
            shift 2
            ;;
        --extra-exclude-regex)
            EXTRA_EXCLUDE_RE="$2"
            shift 2
            ;;
        --no-build)
            DO_BUILD=0
            shift
            ;;
        --skip-boundary-report)
            BOUNDARY_REPORT=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$JOBS" in
    ""|*[!0-9]*)
        echo "invalid --jobs value: $JOBS" >&2
        exit 2
        ;;
esac

if [ ! -d "$BUILD_DIR" ]; then
    echo "build directory not found: $BUILD_DIR" >&2
    echo "configure first, for example: cmake -S $PROJECT_DIR -B $BUILD_DIR" >&2
    exit 2
fi

echo "=== Xray 0.9 local consolidation gate ==="
echo "Project:   $PROJECT_DIR"
echo "Build dir: $BUILD_DIR"
echo "Jobs:      $JOBS"
echo "Excluded known AOT boundary suites: $KNOWN_AOT_BOUNDARY_RE"
GATE_EXCLUDE_RE="$KNOWN_AOT_BOUNDARY_RE"
if [ -n "$EXTRA_EXCLUDE_RE" ]; then
    GATE_EXCLUDE_RE="$GATE_EXCLUDE_RE|$EXTRA_EXCLUDE_RE"
    echo "Extra platform exclusions: $EXTRA_EXCLUDE_RE"
fi
echo ""

if [ "$DO_BUILD" -eq 1 ]; then
    cmake --build "$BUILD_DIR" -j "$JOBS"
    echo ""
fi

"$CTEST" --test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS" \
    -E "$GATE_EXCLUDE_RE"

if [ "$BOUNDARY_REPORT" -eq 1 ]; then
    echo ""
    echo "=== Known open AOT boundary report (non-blocking for 0.9) ==="
    set +e
    "$CTEST" --test-dir "$BUILD_DIR" --output-on-failure -R "$KNOWN_AOT_BOUNDARY_RE"
    boundary_status=$?
    set -e
    if [ "$boundary_status" -eq 0 ]; then
        echo "Known boundary suites now pass; remove the 0.9 exclusion before release."
    else
        echo "Known boundary suites still fail as expected for the current roadmap boundary."
    fi
fi
