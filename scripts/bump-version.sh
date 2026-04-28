#!/usr/bin/env bash
# bump-version.sh - Update Xray version across xray, xray-vscode, xray-website.
#
# Usage:
#   scripts/bump-version.sh <new-version>            # e.g. 0.5.3
#   scripts/bump-version.sh <new-version> --no-commit
#   scripts/bump-version.sh <new-version> --dry-run
#
# Single source of truth: xray/CMakeLists.txt -> project(Xray VERSION x.y.z).
# This script keeps the following files in sync:
#
#   xray/CMakeLists.txt          project(Xray VERSION x.y.z)
#   xray-vscode/package.json     "version": "x.y.z"
#   xray-website/package.json    "version": "x.y.z"
#
# The xray-website displayed version is injected at build time from
# CMakeLists.txt by vite.config.ts, so no website source files need
# to change here. Bumping its package.json is for npm metadata only.
#
# By default the script:
#   - Verifies each affected repo has a clean working tree.
#   - Edits the files in place.
#   - Creates one git commit per repo (use --no-commit to skip).
#   - Never pushes -- review and push manually.

set -euo pipefail

# Make sure the standard tools (dirname, sed, grep, git, python3) resolve
# even when invoked from an environment with a stripped PATH.
export PATH="/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin:${PATH:-}"

# ---------- args ----------
NEW_VER=""
DO_COMMIT=1
DRY_RUN=0

while (( $# > 0 )); do
    case "$1" in
        --no-commit) DO_COMMIT=0 ;;
        --dry-run)   DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,28p' "$0"
            exit 0
            ;;
        -*)
            echo "Unknown flag: $1" >&2
            exit 2
            ;;
        *)
            if [[ -n "$NEW_VER" ]]; then
                echo "Unexpected positional arg: $1" >&2
                exit 2
            fi
            NEW_VER="$1"
            ;;
    esac
    shift
done

if [[ -z "$NEW_VER" ]]; then
    echo "usage: scripts/bump-version.sh <new-version> [--no-commit] [--dry-run]" >&2
    exit 2
fi

if ! [[ "$NEW_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: version must be MAJOR.MINOR.PATCH (got: $NEW_VER)" >&2
    exit 2
fi

# ---------- repo locations ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XRAY_REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$XRAY_REPO/.." && pwd)"
VSCODE_REPO="$WORKSPACE_ROOT/xray-vscode"
WEBSITE_REPO="$WORKSPACE_ROOT/xray-website"

CMAKE_FILE="$XRAY_REPO/CMakeLists.txt"
VSCODE_PKG="$VSCODE_REPO/package.json"
WEBSITE_PKG="$WEBSITE_REPO/package.json"

# ---------- helpers ----------
note()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m!!\033[0m  %s\n' "$*" >&2; }
fatal() { printf '\033[1;31mERR\033[0m %s\n' "$*" >&2; exit 1; }

require_clean() {
    local repo="$1" name="$2"
    [[ -d "$repo/.git" ]] || { warn "$name: not a git repo at $repo (skipping clean check)"; return; }
    if [[ -n "$(git -C "$repo" status --porcelain)" ]]; then
        fatal "$name has uncommitted changes ($repo). Commit/stash first."
    fi
}

read_current_cmake_version() {
    # project(...) can span multiple lines in CMakeLists.txt, so use a
    # multi-line regex via python instead of grep.
    python3 - "$CMAKE_FILE" <<'PY'
import re, sys, pathlib
text = pathlib.Path(sys.argv[1]).read_text()
m = re.search(r'project\s*\(\s*Xray\s+VERSION\s+(\d+\.\d+\.\d+)',
              text, flags=re.IGNORECASE)
if m:
    print(m.group(1))
PY
}

bump_cmake() {
    local cur="$1" new="$2"
    if [[ "$cur" == "$new" ]]; then
        note "CMakeLists.txt already at $new (skip)"
        return
    fi
    note "CMakeLists.txt: $cur -> $new"
    (( DRY_RUN )) && return
    # Match: project(Xray VERSION x.y.z)  with flexible whitespace.
    # Use a placeholder line rebuild to avoid sed dialect differences.
    python3 - "$CMAKE_FILE" "$new" <<'PY'
import re, sys, pathlib
path, new = pathlib.Path(sys.argv[1]), sys.argv[2]
text = path.read_text()
new_text, n = re.subn(
    r'(project\s*\(\s*Xray\s+VERSION\s+)\d+\.\d+\.\d+',
    rf'\g<1>{new}', text, count=1)
if n != 1:
    sys.exit(f"failed to rewrite project(Xray VERSION ...) in {path}")
path.write_text(new_text)
PY
}

bump_pkg_json() {
    local file="$1" label="$2" new="$3"
    [[ -f "$file" ]] || { warn "$label: $file not found (skip)"; return; }
    local cur
    cur="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("version",""))' "$file")"
    if [[ "$cur" == "$new" ]]; then
        note "$label: package.json already at $new (skip)"
        return
    fi
    note "$label: package.json $cur -> $new"
    (( DRY_RUN )) && return
    python3 - "$file" "$new" <<'PY'
import json, sys, pathlib
path, new = pathlib.Path(sys.argv[1]), sys.argv[2]
data = json.loads(path.read_text())
data["version"] = new
# Preserve trailing newline + 2-space indent (npm/pnpm convention).
path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
PY
}

commit_repo() {
    local repo="$1" label="$2" file="$3" new="$4"
    [[ -d "$repo/.git" ]] || { warn "$label: not a git repo, skip commit"; return; }
    if [[ -z "$(git -C "$repo" status --porcelain -- "$file")" ]]; then
        note "$label: nothing to commit"
        return
    fi
    if (( ! DO_COMMIT )); then
        note "$label: changes staged in working tree (commit skipped)"
        return
    fi
    if (( DRY_RUN )); then
        note "$label: [dry-run] would commit $(basename "$file")"
        return
    fi
    note "$label: committing"
    git -C "$repo" add -- "$file"
    git -C "$repo" commit -m "chore: bump version to $new"
}

# ---------- preflight ----------
note "Target version: $NEW_VER"
(( DRY_RUN )) && note "(dry-run mode: no files will be modified)"

[[ -f "$CMAKE_FILE" ]] || fatal "CMakeLists.txt not found at $CMAKE_FILE"
CUR_VER="$(read_current_cmake_version)"
[[ -n "$CUR_VER" ]] || fatal "could not parse current version from $CMAKE_FILE"
note "Current version (CMakeLists.txt): $CUR_VER"

if (( DO_COMMIT )) && (( ! DRY_RUN )); then
    require_clean "$XRAY_REPO"    "xray"
    require_clean "$VSCODE_REPO"  "xray-vscode"
    require_clean "$WEBSITE_REPO" "xray-website"
fi

# ---------- apply ----------
bump_cmake     "$CUR_VER" "$NEW_VER"
bump_pkg_json  "$VSCODE_PKG"  "xray-vscode"  "$NEW_VER"
bump_pkg_json  "$WEBSITE_PKG" "xray-website" "$NEW_VER"

commit_repo "$XRAY_REPO"    "xray"         "$CMAKE_FILE"  "$NEW_VER"
commit_repo "$VSCODE_REPO"  "xray-vscode"  "$VSCODE_PKG"  "$NEW_VER"
commit_repo "$WEBSITE_REPO" "xray-website" "$WEBSITE_PKG" "$NEW_VER"

note "Done."
echo
echo "Next steps:"
echo "  - Review:  git -C $XRAY_REPO    log -1"
echo "             git -C $VSCODE_REPO  log -1"
echo "             git -C $WEBSITE_REPO log -1"
echo "  - Push when ready (each repo separately):"
echo "             git -C <repo> push"
