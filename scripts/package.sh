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

需要系统安装 C 编译器（gcc/clang）：
\`\`\`bash
xray build app.xr -o myapp
\`\`\`

## 目录结构
\`\`\`
xray/
├── bin/xray              # 解释器
├── lib/libxray_core.a    # 运行时库（用于 xray build）
├── lib/xray/stdlib/      # 标准库
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
