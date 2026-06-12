#!/bin/sh
# Install a pinned Zig binary for Xray AOT cross-target smoke tests.
#
# Defaults:
#   install dir:  ~/.local/opt/zig/zig-<arch>-<os>-<version>
#   shim:         ~/.local/bin/zig
#   download dir: ~/.cache/xray-tools/zig
#
# Overrides:
#   ZIG_VERSION=0.16.0
#   XRAY_ZIG_PREFIX=$HOME/.local/opt/zig
#   XRAY_ZIG_BIN_DIR=$HOME/.local/bin
#   XRAY_TOOL_CACHE=$HOME/.cache/xray-tools/zig

set -eu

ZIG_VERSION="${ZIG_VERSION:-0.16.0}"
ZIG_PREFIX="${XRAY_ZIG_PREFIX:-$HOME/.local/opt/zig}"
ZIG_BIN_DIR="${XRAY_ZIG_BIN_DIR:-$HOME/.local/bin}"
ZIG_CACHE="${XRAY_TOOL_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/xray-tools/zig}"

host_os="$(uname -s)"
host_arch="$(uname -m)"

case "$host_os" in
    Darwin) zig_os="macos" ;;
    Linux) zig_os="linux" ;;
    *)
        echo "error: unsupported host OS for Zig installer: $host_os" >&2
        exit 2
        ;;
esac

case "$host_arch" in
    arm64|aarch64) zig_arch="aarch64" ;;
    x86_64|amd64) zig_arch="x86_64" ;;
    *)
        echo "error: unsupported host arch for Zig installer: $host_arch" >&2
        exit 2
        ;;
esac

pkg="zig-${zig_arch}-${zig_os}-${ZIG_VERSION}"
archive="${pkg}.tar.xz"
url="https://ziglang.org/download/${ZIG_VERSION}/${archive}"
install_dir="${ZIG_PREFIX}/${pkg}"
archive_path="${ZIG_CACHE}/${archive}"
tmp_dir=""

cleanup() {
    if [ -n "$tmp_dir" ] && [ -d "$tmp_dir" ]; then
        rm -rf "$tmp_dir"
    fi
}
trap cleanup EXIT INT TERM

mkdir -p "$ZIG_PREFIX" "$ZIG_BIN_DIR" "$ZIG_CACHE"

if [ ! -x "${install_dir}/zig" ]; then
    if [ ! -f "$archive_path" ]; then
        echo "Downloading $url"
        if command -v curl >/dev/null 2>&1; then
            curl -fL "$url" -o "$archive_path"
        elif command -v wget >/dev/null 2>&1; then
            wget -O "$archive_path" "$url"
        else
            echo "error: curl or wget is required to download Zig" >&2
            exit 2
        fi
    fi

    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/xray_zig_install.XXXXXX")"
    tar -xJf "$archive_path" -C "$tmp_dir"
    rm -rf "${install_dir}.tmp"
    mv "${tmp_dir}/${pkg}" "${install_dir}.tmp"
    rm -rf "$install_dir"
    mv "${install_dir}.tmp" "$install_dir"
fi

ln -sfn "${install_dir}/zig" "${ZIG_BIN_DIR}/zig"

echo "Installed Zig:"
echo "  version: $("${ZIG_BIN_DIR}/zig" version)"
echo "  binary:  ${ZIG_BIN_DIR}/zig"
echo "  target:  ${install_dir}"
