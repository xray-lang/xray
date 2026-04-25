#!/bin/bash
#
# Xray 语言安装脚本
# 作者：xingleixu@gmail.com
#
# 用法：
#   curl -fsSL https://xray-lang.org/install.sh | bash
#   或
#   wget -qO- https://xray-lang.org/install.sh | bash
#

set -e

# 颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BLUE='\033[0;34m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' NC=''
fi

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# 版本
XRAY_VERSION="${XRAY_VERSION:-0.5.0}"
XRAY_INSTALL_DIR="${XRAY_INSTALL_DIR:-/usr/local/xray}"
XRAY_BASE_URL="${XRAY_BASE_URL:-https://github.com/xray-lang/xray/releases/download}"

# 检测系统
detect_platform() {
    local os arch

    case "$(uname -s)" in
        Darwin) os="macos" ;;
        Linux)  os="linux" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *) error "不支持的操作系统: $(uname -s)" ;;
    esac

    case "$(uname -m)" in
        x86_64|amd64) arch="x86_64" ;;
        arm64|aarch64) arch="arm64" ;;
        *) error "不支持的架构: $(uname -m)" ;;
    esac

    echo "${os}-${arch}"
}

# 检查依赖
check_dependencies() {
    info "检查依赖..."

    # 检查 C 编译器（用于 xray build）
    if command -v cc &> /dev/null || command -v gcc &> /dev/null || command -v clang &> /dev/null; then
        success "C 编译器已安装"
    else
        warn "未检测到 C 编译器（xray build 需要）"
        case "$(uname -s)" in
            Darwin)
                echo "  安装命令: xcode-select --install"
                ;;
            Linux)
                echo "  安装命令: sudo apt install build-essential  # Debian/Ubuntu"
                echo "           sudo yum install gcc              # CentOS/RHEL"
                ;;
        esac
    fi
}

# 下载安装包
download_xray() {
    local platform="$1"
    local url="${XRAY_BASE_URL}/v${XRAY_VERSION}/xray-${XRAY_VERSION}-${platform}.tar.gz"
    local tmp_file="/tmp/xray-${XRAY_VERSION}.tar.gz"

    info "下载 Xray ${XRAY_VERSION} (${platform})..."

    if command -v curl &> /dev/null; then
        curl -fsSL -o "$tmp_file" "$url" || error "下载失败: $url"
    elif command -v wget &> /dev/null; then
        wget -q -O "$tmp_file" "$url" || error "下载失败: $url"
    else
        error "需要 curl 或 wget"
    fi

    echo "$tmp_file"
}

# 安装
install_xray() {
    local archive="$1"

    info "安装到 ${XRAY_INSTALL_DIR}..."

    # 创建目录
    sudo mkdir -p "$XRAY_INSTALL_DIR"

    # 解压
    sudo tar -xzf "$archive" -C "$XRAY_INSTALL_DIR" --strip-components=1

    # 清理
    rm -f "$archive"

    success "安装完成"
}

# 配置环境变量
setup_env() {
    local shell_rc=""
    local shell_name="$(basename "$SHELL")"

    case "$shell_name" in
        bash) shell_rc="$HOME/.bashrc" ;;
        zsh)  shell_rc="$HOME/.zshrc" ;;
        *)    shell_rc="$HOME/.profile" ;;
    esac

    info "配置环境变量..."

    # 检查是否已配置
    if grep -q "XRAY_HOME" "$shell_rc" 2>/dev/null; then
        warn "环境变量已配置"
        return
    fi

    cat >> "$shell_rc" << 'EOF'

# Xray Language
export XRAY_HOME="/usr/local/xray"
export PATH="$XRAY_HOME/bin:$PATH"
export XRAY_INCLUDE="$XRAY_HOME/include"
export XRAY_LIB="$XRAY_HOME/lib"
EOF

    success "已添加到 $shell_rc"
    echo ""
    warn "请运行以下命令使配置生效："
    echo "  source $shell_rc"
}

# 验证安装
verify_install() {
    info "验证安装..."

    if [ -x "${XRAY_INSTALL_DIR}/bin/xray" ]; then
        echo ""
        "${XRAY_INSTALL_DIR}/bin/xray" --version
        echo ""
        success "Xray 安装成功！"
    else
        error "安装验证失败"
    fi
}

# 主流程
main() {
    echo ""
    echo "╔══════════════════════════════════════════╗"
    echo "║       Xray Language Installer            ║"
    echo "║       Version: ${XRAY_VERSION}                      ║"
    echo "╚══════════════════════════════════════════╝"
    echo ""

    check_dependencies

    local platform=$(detect_platform)
    info "检测到平台: $platform"

    local archive=$(download_xray "$platform")
    install_xray "$archive"
    setup_env
    verify_install

    echo ""
    echo "快速开始:"
    echo "  xray --help              # 查看帮助"
    echo "  xray app.xr              # 运行脚本"
    echo "  xray build app.xr        # 编译为可执行文件"
    echo ""
}

main "$@"
