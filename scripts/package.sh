#!/bin/bash
#
# Xray 打包脚本
# 作者：xingleixu@gmail.com
#
# 用法：./scripts/package.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BUNDLE_ZIG="${XRAY_PACKAGE_BUNDLE_ZIG:-auto}"
ZIG_BIN="${XRAY_ZIG:-}"

# 获取版本号
VERSION=$(grep "VERSION 0" "${PROJECT_DIR}/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ -z "$VERSION" ]; then
    VERSION="0.5.0"
fi

# 检测平台
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$OS" in
    darwin) OS="macos" ;;
    linux)  OS="linux" ;;
esac

case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    arm64|aarch64) ARCH="arm64" ;;
esac

PLATFORM="${OS}-${ARCH}"
PACKAGE_NAME="xray-${VERSION}-${PLATFORM}"
PACKAGE_DIR="/tmp/${PACKAGE_NAME}"
OUTPUT_FILE="${BUILD_DIR}/${PACKAGE_NAME}.tar.gz"

echo "============================================"
echo "Xray 打包脚本"
echo "============================================"
echo "版本: ${VERSION}"
echo "平台: ${PLATFORM}"
echo "输出: ${OUTPUT_FILE}"
echo "Bundled Zig: ${BUNDLE_ZIG}"
echo ""

# 确保已编译
if [ ! -f "${BUILD_DIR}/xray" ]; then
    echo "错误: 请先编译 xray"
    echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4"
    exit 1
fi

# 清理旧目录
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"

# 安装到临时目录
echo "安装到临时目录..."
cmake --install "${BUILD_DIR}" --prefix "${PACKAGE_DIR}" > /dev/null

find_zig_for_bundle() {
    if [ -n "$ZIG_BIN" ] && [ -x "$ZIG_BIN" ]; then
        printf '%s\n' "$ZIG_BIN"
        return 0
    fi
    if command -v zig >/dev/null 2>&1; then
        command -v zig
        return 0
    fi
    if [ "$BUNDLE_ZIG" = "0" ] || [ "$BUNDLE_ZIG" = "off" ] || [ "$BUNDLE_ZIG" = "false" ]; then
        return 1
    fi

    echo "未找到 Zig，尝试安装固定 Zig 工具链..." >&2
    "${PROJECT_DIR}/scripts/install_zig_toolchain.sh" >/dev/null
    if command -v zig >/dev/null 2>&1; then
        command -v zig
        return 0
    fi
    if [ -x "$HOME/.local/bin/zig" ]; then
        printf '%s\n' "$HOME/.local/bin/zig"
        return 0
    fi
    return 1
}

resolve_path() {
    local path="$1"
    local link
    local dir

    while [ -L "$path" ]; do
        link="$(readlink "$path")"
        case "$link" in
            /*) path="$link" ;;
            *) path="$(dirname "$path")/$link" ;;
        esac
    done
    dir="$(cd -P "$(dirname "$path")" && pwd)"
    printf '%s/%s\n' "$dir" "$(basename "$path")"
}

bundle_zig() {
    local zig_src
    local zig_real
    local zig_root
    local zig_dst_dir
    local zig_dst

    case "$BUNDLE_ZIG" in
        0|off|false)
            echo "跳过 bundled Zig"
            return 0
            ;;
    esac

    if ! zig_src="$(find_zig_for_bundle)"; then
        echo "警告: 未能找到或安装 Zig，本安装包不包含 cross-target 工具链" >&2
        return 0
    fi

    zig_real="$(resolve_path "$zig_src")"
    zig_root="$(dirname "$zig_real")"
    zig_dst_dir="${PACKAGE_DIR}/libexec/xray/zig"
    zig_dst="${zig_dst_dir}/zig"
    mkdir -p "$zig_dst_dir"
    if [ -d "${zig_root}/lib" ]; then
        (cd "$zig_root" && tar -cf - .) | (cd "$zig_dst_dir" && tar -xf -)
    else
        echo "警告: Zig 根目录缺少 lib/，仅复制 zig 可执行文件可能无法执行 zig cc" >&2
        cp "$zig_real" "$zig_dst"
    fi
    chmod +x "$zig_dst" 2>/dev/null || true
    echo "已打包 Zig: $zig_real -> libexec/xray/zig/"
}

bundle_zig

# 清理多余的头文件（只保留 include/xray/）
rm -f "${PACKAGE_DIR}/include/"*.h 2>/dev/null || true

# 创建 README
cat > "${PACKAGE_DIR}/README.md" << EOF
# Xray ${VERSION}

## 安装

### 自动安装
\`\`\`bash
curl -fsSL https://xray-lang.org/install.sh | bash
\`\`\`

### 手动安装
\`\`\`bash
sudo tar -xzf ${PACKAGE_NAME}.tar.gz -C /usr/local/xray --strip-components=1
\`\`\`

### 配置环境变量
\`\`\`bash
export XRAY_HOME="/usr/local/xray"
export PATH="\$XRAY_HOME/bin:\$PATH"
export XRAY_INCLUDE="\$XRAY_HOME/include/xray"
export XRAY_LIB="\$XRAY_HOME/lib"
\`\`\`

## 编译为可执行文件

本安装包可携带 Zig toolchain，用于 AOT 跨平台交叉编译。可先检查：
\`\`\`bash
xray toolchain doctor
\`\`\`

native build 可使用系统 C 编译器；cross target 默认使用 Zig：
\`\`\`bash
xray build app.xr -o myapp
xray build --native --target x86_64-linux-musl app.xr -o myapp-linux
xray build --native --target x86_64-windows-gnu app.xr -o myapp.exe
\`\`\`

## 目录结构
\`\`\`
xray/
├── bin/xray              # 解释器
├── lib/libxray_aot_core.a # AOT core/direct-call runtime
├── lib/libxray_rt_coro.a  # AOT coroutine/timer runtime
├── lib/libxray_vm_runtime.a # VM bytecode embedding runtime
├── lib/xray/stdlib/      # 标准库
├── libexec/xray/zig/     # 可选 bundled Zig toolchain
└── include/xray/         # 头文件（用于 xray build）
\`\`\`

## 文档

https://xray-lang.org/docs
EOF

# 打包
echo "创建安装包..."
cd /tmp
tar -czf "${OUTPUT_FILE}" "${PACKAGE_NAME}"

# 清理
rm -rf "${PACKAGE_DIR}"

# 输出结果
echo ""
echo "============================================"
echo "打包完成！"
echo "============================================"
echo "文件: ${OUTPUT_FILE}"
echo "大小: $(du -h "${OUTPUT_FILE}" | cut -f1)"
echo ""
echo "测试安装:"
echo "  mkdir -p /tmp/xray-test"
echo "  tar -xzf ${OUTPUT_FILE} -C /tmp/xray-test --strip-components=1"
echo "  /tmp/xray-test/bin/xray --version"
echo "  /tmp/xray-test/bin/xray toolchain doctor"
