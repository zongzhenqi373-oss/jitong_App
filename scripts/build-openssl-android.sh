#!/usr/bin/env bash
#
# 交叉编译 OpenSSL 静态库（Android）。
#
# 为什么需要：Android NDK 不提供 OpenSSL，而 client_core 的 TcpTransport 走
# asio::ssl。P1 首阶段就要能编出 libjitong_kernel.so，因此需要预编译静态库。
#
# 用法：
#   ./scripts/build-openssl-android.sh [abi] [api] [openssl_version]
#
#   abi              arm64-v8a | x86_64        （默认 arm64-v8a）
#   api              Android API level          （默认 33，等于 minSdk）
#   openssl_version  OpenSSL 源码版本           （默认 3.0.15，LTS）
#
# 产物布局（被 app/src/main/cpp/CMakeLists.txt 的 CMAKE_PREFIX_PATH 消费）：
#   app/src/main/cpp/third_party/android/<abi>/include/openssl/...
#   app/src/main/cpp/third_party/android/<abi>/lib/libssl.a
#   app/src/main/cpp/third_party/android/<abi>/lib/libcrypto.a
#
set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${2:-33}"
OPENSSL_VERSION="${3:-3.0.15}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${JT_NATIVE_WORK:-/tmp/jt-native}"
SRC_DIR="${WORK_DIR}/src/openssl-${OPENSSL_VERSION}"
OUT_ROOT="${JT_NATIVE_OUT:-${REPO_ROOT}/jitong_android/app/src/main/cpp/third_party/android}"
OUT_DIR="${OUT_ROOT}/${ABI}"

ANDROID_SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    # 取版本号最大的已安装 NDK
    ANDROID_NDK_HOME="$(ls -d "${ANDROID_SDK_ROOT}"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
    export ANDROID_NDK_HOME
fi
if [[ -z "${ANDROID_NDK_HOME}" || ! -d "${ANDROID_NDK_HOME}" ]]; then
    echo "error: 找不到 NDK，请先安装：sdkmanager --install 'ndk;27.3.13750724'" >&2
    exit 1
fi

case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
    Linux)  HOST_TAG="linux-x86_64" ;;
    *)      echo "error: 不支持的宿主系统 $(uname -s)" >&2; exit 1 ;;
esac

case "${ABI}" in
    arm64-v8a) OPENSSL_TARGET="android-arm64" ;;
    x86_64)    OPENSSL_TARGET="android-x86_64" ;;
    *)         echo "error: 不支持的 ABI ${ABI}" >&2; exit 1 ;;
esac

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
if [[ ! -d "${TOOLCHAIN}" ]]; then
    echo "error: 找不到 NDK toolchain: ${TOOLCHAIN}" >&2
    exit 1
fi
export PATH="${TOOLCHAIN}/bin:${PATH}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_HOME}"

echo "==> OpenSSL ${OPENSSL_VERSION} for ${ABI} (API ${API})"
echo "    NDK      : ${ANDROID_NDK_HOME}"
echo "    output   : ${OUT_DIR}"

# 下载并解压源码
mkdir -p "${WORK_DIR}/src"
if [[ ! -d "${SRC_DIR}" ]]; then
    echo "==> 下载 OpenSSL 源码"
    curl -L --retry 3 -o "${WORK_DIR}/src/openssl-${OPENSSL_VERSION}.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    tar -xzf "${WORK_DIR}/src/openssl-${OPENSSL_VERSION}.tar.gz" -C "${WORK_DIR}/src"
fi

# OpenSSL 不支持 out-of-tree 构建，复制到独立目录以免污染源码树
BUILD_DIR="${WORK_DIR}/build/openssl-${OPENSSL_VERSION}-${ABI}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cp -R "${SRC_DIR}/." "${BUILD_DIR}/"

pushd "${BUILD_DIR}" > /dev/null

# no-shared：只出静态库，避免打包 .so 与运行时查找问题
# no-tests：跳过测试程序，显著缩短构建时间
# 注意：no-apps / no-docs 是 OpenSSL 3.1+ 的选项，3.0.x 传入会直接报
# "Unsupported options" 并终止配置，这里保持 3.0 兼容的最小集合。
./Configure "${OPENSSL_TARGET}" \
    -D__ANDROID_API__="${API}" \
    --prefix="${OUT_DIR}" \
    --openssldir="${OUT_DIR}/ssl" \
    no-shared \
    no-tests

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
make install_sw

popd > /dev/null

echo "==> 完成，校验产物："
ls -l "${OUT_DIR}/lib/libssl.a" "${OUT_DIR}/lib/libcrypto.a"
echo "    ${ABI} OpenSSL 就绪"
