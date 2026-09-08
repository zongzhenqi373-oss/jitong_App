#!/usr/bin/env bash
#
# 交叉编译 protobuf C++ 运行库（Android）。
#
# 版本必须与宿主机 protoc 主版本一致：im.pb.cc 由宿主机 protoc 生成，
# 链接的目标机运行库版本不匹配会直接编译失败或运行期崩溃。
# 仓库桌面端使用 Homebrew protobuf（见 `protoc --version`）。
#
# 用法：
#   ./scripts/build-protobuf-android.sh [abi] [api] [protobuf_version]
#
#   abi               arm64-v8a | x86_64   （默认 arm64-v8a）
#   api               Android API level    （默认 33）
#   protobuf_version  protobuf 版本        （默认跟随宿主机 protoc 主版本）
#
# 产物布局：
#   app/src/main/cpp/third_party/android/<abi>/include/google/protobuf/...
#   app/src/main/cpp/third_party/android/<abi>/lib/libprotobuf.a (+ abseil 静态库)
#
set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${2:-33}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${JT_NATIVE_WORK:-/tmp/jt-native}"
OUT_ROOT="${JT_NATIVE_OUT:-${REPO_ROOT}/jitong_android/app/src/main/cpp/third_party/android}"
OUT_DIR="${OUT_ROOT}/${ABI}"

# 默认跟随宿主机 protoc 版本，保证生成物与运行库同主版本
HOST_PROTOC="${IM_PROTOC_EXECUTABLE:-$(command -v protoc || true)}"
if [[ -z "${HOST_PROTOC}" ]]; then
    echo "error: 找不到宿主机 protoc，请先安装 protobuf 或设置 IM_PROTOC_EXECUTABLE" >&2
    exit 1
fi
HOST_PROTOBUF_VERSION="$("${HOST_PROTOC}" --version | awk '{print $2}')"
PROTOBUF_VERSION="${3:-${HOST_PROTOBUF_VERSION}}"

echo "==> 宿主机 protoc: ${HOST_PROTOC} (${HOST_PROTOBUF_VERSION})"
echo "==> 目标 protobuf: ${PROTOBUF_VERSION}"

ANDROID_SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
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

SRC_DIR="${WORK_DIR}/src/protobuf-${PROTOBUF_VERSION}"
BUILD_DIR="${WORK_DIR}/build/protobuf-${PROTOBUF_VERSION}-${ABI}"

echo "==> protobuf ${PROTOBUF_VERSION} for ${ABI} (API ${API})"
echo "    NDK    : ${ANDROID_NDK_HOME}"
echo "    output : ${OUT_DIR}"

mkdir -p "${WORK_DIR}/src"
if [[ ! -d "${SRC_DIR}" ]]; then
    echo "==> 下载 protobuf 源码"
    curl -L --retry 3 -o "${WORK_DIR}/src/protobuf-${PROTOBUF_VERSION}.tar.gz" \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-${PROTOBUF_VERSION}.tar.gz"
    tar -xzf "${WORK_DIR}/src/protobuf-${PROTOBUF_VERSION}.tar.gz" -C "${WORK_DIR}/src"
fi

cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="android-${API}" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${OUT_DIR}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_CONFORMANCE=OFF \
    -Dprotobuf_BUILD_EXAMPLES=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_WITH_ZLIB=OFF \
    -Dprotobuf_MSVC_STATIC_RUNTIME=OFF \
    -Dprotobuf_ABSL_PROVIDER=module \
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
    -Dprotobuf_DISABLE_RTTI=OFF \
    -DProtobuf_PROTOC_EXECUTABLE="${HOST_PROTOC}"

cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
cmake --install "${BUILD_DIR}"

echo "==> 完成，校验产物："
ls -l "${OUT_DIR}/lib" | grep -E 'libprotobuf|libabsl|libutf8' || true
echo "    ${ABI} protobuf 就绪"
