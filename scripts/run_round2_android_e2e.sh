#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${HOME}/Library/Android/sdk}"
ADB="${ANDROID_SDK_ROOT}/platform-tools/adb"
JAVA_HOME="${JITONG_JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
PORT="${JITONG_E2E_PORT:-24680}"
HOST="${JITONG_E2E_HOST:-10.0.2.2}"
SERVER_NAME="${JITONG_E2E_SERVER_NAME:-im.example.com}"
CONFIG_DIR="${JITONG_E2E_CONFIG_DIR:-${REPO_ROOT}/build/im-server/config}"
LOG_FILE="${JITONG_E2E_LOG:-${REPO_ROOT}/outputs/kernel-round2-android-socket-e2e.log}"
CA_FILE="${CONFIG_DIR}/test_server.crt"
IDENTITY_KEY="${CONFIG_DIR}/test_app_identity_private.pem"
DEVICE_SERIAL="${ANDROID_SERIAL:-$($ADB devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')}"

test -x "$ADB" || { echo "adb 不存在: $ADB" >&2; exit 2; }
test -f "$CA_FILE" || { echo "测试 CA 不存在: $CA_FILE" >&2; exit 3; }
test -f "$IDENTITY_KEY" || { echo "应用身份私钥不存在: $IDENTITY_KEY" >&2; exit 4; }

test -n "$DEVICE_SERIAL" || { echo "没有可用 Android 设备" >&2; exit 5; }
ADB_DEVICE=("$ADB" -s "$DEVICE_SERIAL")
ABI="$("${ADB_DEVICE[@]}" shell getprop ro.product.cpu.abi | tr -d '\r')"
test "$ABI" = "arm64-v8a" || { echo "需要 arm64-v8a 设备，当前为: $ABI" >&2; exit 5; }

CA_BASE64="$(base64 < "$CA_FILE" | tr -d '\r\n')"
IDENTITY_PUBLIC_KEY="$(openssl pkey -in "$IDENTITY_KEY" -pubout -outform DER 2>/dev/null | tail -c 32 | base64 | tr -d '\r\n')"
SPKI_PIN="$(openssl x509 -in "$CA_FILE" -pubkey -noout 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null | openssl dgst -sha256 -binary | base64 | tr -d '\r\n')"
MARKER="${JITONG_E2E_MARKER:-JITONG_F06_$(date +%s)_$RANDOM}"

echo "运行 F05：device=$DEVICE_SERIAL abi=$ABI host=$HOST:$PORT serverName=$SERVER_NAME"
"${ADB_DEVICE[@]}" logcat -c
cd "$REPO_ROOT/jitong_android"
ANDROID_SERIAL="$DEVICE_SERIAL" JAVA_HOME="$JAVA_HOME" ./gradlew :app:connectedDebugAndroidTest -x lint \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.NativeSocketHeartbeatTest \
  -Pandroid.testInstrumentationRunnerArguments.jitongHost="$HOST" \
  -Pandroid.testInstrumentationRunnerArguments.jitongPort="$PORT" \
  -Pandroid.testInstrumentationRunnerArguments.jitongServerName="$SERVER_NAME" \
  -Pandroid.testInstrumentationRunnerArguments.jitongCaBase64="$CA_BASE64" \
  -Pandroid.testInstrumentationRunnerArguments.jitongIdentityPublicKey="$IDENTITY_PUBLIC_KEY" \
  -Pandroid.testInstrumentationRunnerArguments.jitongSpkiPin="$SPKI_PIN" \
  -Pandroid.testInstrumentationRunnerArguments.jitongMarker="$MARKER" 2>&1 | tee "$LOG_FILE"

"${ADB_DEVICE[@]}" logcat -d -s JitongRound2E2E JitongKernel | tee -a "$LOG_FILE"
echo "F05 日志：$LOG_FILE"
echo "F06 marker：$MARKER"
