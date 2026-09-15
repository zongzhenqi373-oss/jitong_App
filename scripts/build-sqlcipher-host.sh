#!/usr/bin/env bash
# 生成本机（桌面/CI）可用的 SQLCipher amalgamation（sqlite3.c / sqlite3.h）。
#
# 用途：桌面侧 CLIENT_CORE_WITH_SQLCIPHER=ON 时需要编译 SQLCipher 源码，以便在
#       macOS/Linux 上跑 C++ 单元测试（CipherDatabase / Schema / 迁移 等）。
#
# Android 侧**不需要**本脚本：按 outputs/kernel-round5-encryption-note.md 的 S0 决策，
# Android 走「方案 A」——直接链接 Room 自带的 libsqlcipher.so（AAR 内 jni/<abi>/），
# 版本与 Room 的 sqlcipher-android 4.6.1 完全一致，无额外体积、无符号冲突。
#
# 版本必须与 app/build.gradle.kts 中的 net.zetetic:sqlcipher-android 保持一致。
#
# 用法：
#   bash scripts/build-sqlcipher-host.sh            # 默认 4.6.1
#   SQLCIPHER_VERSION=4.6.2 bash scripts/build-sqlcipher-host.sh
set -euo pipefail

SQLCIPHER_VERSION="${SQLCIPHER_VERSION:-4.6.1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/client_core/third_party/sqlcipher"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# 主机 OpenSSL 前缀（SQLCipher 4.x 用 OpenSSL 作 crypto backend）
if command -v brew >/dev/null 2>&1 && [ -d "$(brew --prefix openssl@3 2>/dev/null || true)" ]; then
    OPENSSL_PREFIX="$(brew --prefix openssl@3)"
elif [ -d /usr/local/opt/openssl@3 ]; then
    OPENSSL_PREFIX=/usr/local/opt/openssl@3
elif [ -d /usr/include/openssl ] || [ -d /usr/local/include/openssl ]; then
    OPENSSL_PREFIX=""
else
    echo "[错误] 找不到 OpenSSL 开发库，请先安装（brew install openssl@3）" >&2
    exit 1
fi

echo "[1/4] 下载 SQLCipher ${SQLCIPHER_VERSION} ..."
TARBALL="${WORK}/sqlcipher-${SQLCIPHER_VERSION}.tar.gz"
curl -fSL --max-time 300 -o "${TARBALL}" \
    "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v${SQLCIPHER_VERSION}.tar.gz"

echo "[2/4] 解压 ..."
mkdir -p "${WORK}/src"
tar -xzf "${TARBALL}" -C "${WORK}/src" --strip-components=1

echo "[3/4] configure + 生成 amalgamation ..."
cd "${WORK}/src"
if [ -n "${OPENSSL_PREFIX}" ]; then
    CFLAGS_EXTRA="-I${OPENSSL_PREFIX}/include"
    LDFLAGS_EXTRA="-L${OPENSSL_PREFIX}/lib"
else
    CFLAGS_EXTRA=""
    LDFLAGS_EXTRA=""
fi
./configure \
    --with-crypto-lib=openssl \
    --enable-tempstore=yes \
    --disable-tcl \
    CFLAGS="-DSQLITE_HAS_CODEC ${CFLAGS_EXTRA}" \
    LDFLAGS="${LDFLAGS_EXTRA}" >"${WORK}/configure.log" 2>&1 \
    || { echo "[错误] configure 失败，日志：${WORK}/configure.log"; tail -30 "${WORK}/configure.log"; exit 1; }

make sqlite3.c >"${WORK}/make.log" 2>&1 \
    || { echo "[错误] 生成 amalgamation 失败，日志：${WORK}/make.log"; tail -30 "${WORK}/make.log"; exit 1; }

echo "[4/4] 安装到 ${DEST} ..."
mkdir -p "${DEST}"
cp "${WORK}/src/sqlite3.c" "${DEST}/sqlite3.c"
cp "${WORK}/src/sqlite3.h" "${DEST}/sqlite3.h"

echo "完成："
ls -l "${DEST}/sqlite3.c" "${DEST}/sqlite3.h"
echo
echo "提示：这两个文件是构建产物（体积较大），已被 .gitignore 忽略；"
echo "      新环境请先运行本脚本再开启 CLIENT_CORE_WITH_SQLCIPHER。"
