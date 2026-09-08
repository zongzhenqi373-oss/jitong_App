#!/usr/bin/env bash
# C++ 原生代码动态安全测试：ASan + UBSan（Apple Clang / Linux Clang/GCC）。
#
# 默认测试 client_core 与 im_server；使用独立 build-sanitize 目录，不污染普通构建。
#   bash scripts/native_sanitizer_test.sh
#   bash scripts/native_sanitizer_test.sh --client-only
#   bash scripts/native_sanitizer_test.sh --server-only
#   bash scripts/native_sanitizer_test.sh --with-macos-leaks
#
# 注意：客户端 integration 会绑定本机回环端口；受限沙箱中运行时需授权网络监听。
# 此脚本覆盖 C/C++ 内存安全；Android/Kotlin 的对象泄漏请用 Android Profiler 或
# LeakCanary 在模拟器/真机上验证，不能由 ASan 替代。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_CLIENT=1
RUN_SERVER=1
RUN_MACOS_LEAKS=0
FAIL_COUNT=0
PASS_COUNT=0
WARN_COUNT=0

if [[ -t 1 ]]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''
fi

usage() {
  sed -n '1,11p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
  case "$arg" in
    --client-only) RUN_CLIENT=1; RUN_SERVER=0 ;;
    --server-only) RUN_CLIENT=0; RUN_SERVER=1 ;;
    --with-macos-leaks) RUN_MACOS_LEAKS=1 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "未知参数：$arg（可用 --help 查看）" >&2; exit 2 ;;
  esac
done

pass() { printf '%s[PASS]%s %s\n' "$GREEN$BOLD" "$RESET" "$*"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { printf '%s[FAIL]%s %s\n' "$RED$BOLD" "$RESET" "$*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
warn() { printf '%s[WARN]%s %s\n' "$YELLOW$BOLD" "$RESET" "$*"; WARN_COUNT=$((WARN_COUNT + 1)); }
info() { printf '%s[INFO]%s %s\n' "$YELLOW" "$RESET" "$*"; }

if ! command -v cmake >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
  echo '需要 cmake 与 C++ 编译器（clang 或 gcc）。' >&2
  exit 2
fi

# CMake 的编译与链接阶段都必须带 sanitizer 标志；独立目录确保不会混入普通对象文件。
SAN_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
ASAN_OPTIONS_VALUE='detect_leaks=1:halt_on_error=0:allocator_may_return_null=1'
UBSAN_OPTIONS_VALUE='print_stacktrace=1:halt_on_error=0'
IS_MACOS=0
[[ "$(uname -s)" == 'Darwin' ]] && IS_MACOS=1
if [[ "$IS_MACOS" -eq 1 ]]; then
  # Apple Clang 的 AddressSanitizer 不实现 LeakSanitizer；detect_leaks=1 会让测试
  # 直接失败。越界/UAF/双重释放仍由 ASan 覆盖，泄漏改由 --with-macos-leaks 的
  # 系统 leaks 工具检测。
  ASAN_OPTIONS_VALUE='halt_on_error=0:allocator_may_return_null=1'
fi

run_macos_leaks() {
  local label="$1" source_dir="$2" leaks_build="$3"; shift 3
  local test_name log_dir log_file
  if [[ "$RUN_MACOS_LEAKS" -ne 1 ]]; then return; fi
  if [[ "$IS_MACOS" -ne 1 ]]; then
    info '--with-macos-leaks 仅适用于 macOS；Linux 已由 ASan detect_leaks 覆盖。'
    return
  fi
  if ! command -v leaks >/dev/null 2>&1; then
    fail "${label}：未找到 macOS leaks 工具。请安装 Xcode Command Line Tools 后重试。"
    return
  fi
  if ! command -v codesign >/dev/null 2>&1; then
    warn "${label}：未找到 codesign，无法为临时 leaks 测试二进制授予调试权限。"
    return
  fi
  # leaks 无法分析 ASan 的替代分配器，所以专门构建一份未注入 sanitizer 的
  # Debug 可执行文件。它与 build-sanitize 完全隔离，不影响 ASan/UBSan 结果。
  info "${label}：配置未注入 ASan 的 leaks 专用 Debug 构建"
  if ! cmake -S "$source_dir" -B "$leaks_build" -DCMAKE_BUILD_TYPE=Debug; then
    fail "${label}：leaks 专用构建配置失败。"
    return
  fi
  if ! cmake --build "$leaks_build" --parallel; then
    fail "${label}：leaks 专用构建失败。"
    return
  fi
  log_dir="$(mktemp -d "${TMPDIR:-/tmp}/jitong-leaks.XXXXXX")"
  for test_name in "$@"; do
    log_file="$log_dir/$test_name.log"
    info "${label}：leaks 检测 ${test_name}"
    # CMake 生成的命令行测试程序默认未带 get-task-allow；仅对 build-leaks
    # 内的临时 Debug 二进制做 ad-hoc 签名，绝不接触正常 build 或发布产物。
    if ! codesign --force --sign - --entitlements "$ROOT/scripts/macos_leaks_debug.entitlements" \
        "$leaks_build/$test_name" >/dev/null 2>&1; then
      warn "${label}：无法为 ${test_name} 添加临时调试签名，跳过 leaks（日志：${log_file}）。"
      continue
    fi
    # leaks 的退出码不能单独代表“零泄漏”，所以依据它的标准汇总行判断。
    leaks --atExit -- "$leaks_build/$test_name" >"$log_file" 2>&1
    local leaks_rc=$?
    if grep -E -q "Couldn't get task port|not debuggable|unable to inspect heap ranges" "$log_file"; then
      warn "${label}：macOS 拒绝 leaks 的进程调试权限，未完成 ${test_name} 泄漏检测（日志：${log_file}）。请在普通 Terminal 运行，或在系统设置中允许 Terminal/IDE 的 Developer Tools 权限。"
    elif [[ "$leaks_rc" -ne 0 ]]; then
      fail "${label}：leaks 无法执行 ${test_name}（日志：${log_file}）。"
    elif grep -E -q '0 leaks for 0 total leaked bytes' "$log_file"; then
      pass "${label}：${test_name} 未报告可达内存泄漏。"
    else
      fail "${label}：${test_name} 发现泄漏或 leaks 输出无法确认（日志：${log_file}）。"
      tail -n 24 "$log_file"
    fi
  done
}

run_suite() {
  local label="$1" source_dir="$2" build_dir="$3" ctest_filter="$4"
  info "${label}：配置 ASan + UBSan 构建"
  if ! cmake -S "$source_dir" -B "$build_dir" \
      -DCMAKE_BUILD_TYPE=Debug \
      "-DCMAKE_CXX_FLAGS=${SAN_FLAGS}" \
      "-DCMAKE_EXE_LINKER_FLAGS=${SAN_FLAGS}"; then
    fail "${label}：Sanitizer 配置失败。"
    return
  fi
  if ! cmake --build "$build_dir" --parallel; then
    fail "${label}：Sanitizer 构建失败。"
    return
  fi

  info "${label}：执行测试（ASAN_OPTIONS=${ASAN_OPTIONS_VALUE}）"
  if env ASAN_OPTIONS="$ASAN_OPTIONS_VALUE" UBSAN_OPTIONS="$UBSAN_OPTIONS_VALUE" \
      ctest --test-dir "$build_dir" -R "$ctest_filter" --output-on-failure; then
    pass "${label}：测试通过，未报告 ASan/UBSan 内存或未定义行为错误。"
  else
    fail "${label}：测试失败或 Sanitizer 报错；请查看上方堆栈并按报错文件定位。"
  fi
}

printf '%sC++ 内存与未定义行为测试%s\n' "$BOLD" "$RESET"
info "仓库：$ROOT"
info "编译器：$(c++ --version | head -n 1)"
info '覆盖：堆/栈越界、use-after-free、double-free、部分内存泄漏、整数/类型等未定义行为。'
if [[ "$IS_MACOS" -eq 1 && "$RUN_MACOS_LEAKS" -eq 0 ]]; then
  info 'macOS 的 ASan 不含 LeakSanitizer；如需泄漏检测，请追加 --with-macos-leaks。'
fi

if [[ "$RUN_CLIENT" -eq 1 ]]; then
  # 第二轮新增 P3/P4：frame_codec（线格式边界）、transport（回环 TLS 生命周期/重连）、
  # secure_channel（四步握手 + 加密帧 + 负向）也必须纳入 ASan/UBSan。
  run_suite 'client_core（协议、存储、回环网络、帧编解码、Transport、安全通道）' \
    "$ROOT/client_core" "$ROOT/client_core/build-sanitize" \
    '^(protocol|storage|integration|frame_codec|transport|secure_channel)$'
  run_macos_leaks 'client_core' "$ROOT/client_core" "$ROOT/client_core/build-leaks" \
    test_protocol test_storage test_integration test_frame_codec test_transport test_secure_channel
fi

if [[ "$RUN_SERVER" -eq 1 ]]; then
  # 第二轮：C++ ClientSecureChannel 完成后，完整业务 e2e 已解除 DISABLED，必须纳入
  # ASan/UBSan；security_e2e 继续验证旧客户端跳过应用握手时 fail-close。
  run_suite 'im_server（完整业务 e2e + 安全 e2e + 单元/并发/协议）' \
    "$ROOT/im_server" "$ROOT/im_server/build-sanitize" \
    '^(e2e|security_e2e|database_concurrency|token_service|login_rate_limiter|device_proof|db_write_queue|conversation_migration|protocol_router|ai_model_client|ai_context|ai_request_limiter|app_crypto)$'
  run_macos_leaks 'im_server' "$ROOT/im_server" "$ROOT/im_server/build-leaks" test_e2e
fi

printf '\n结果：PASS=%s WARN=%s FAIL=%s\n' "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"
if [[ "$FAIL_COUNT" -gt 0 ]]; then
  printf '%s结论：未通过。%s 测试失败不一定都是内存漏洞，也可能是端口占用或环境依赖；优先查看 ASan/UBSan 堆栈。\n' "$RED$BOLD" "$RESET"
  exit 1
fi
if [[ "$WARN_COUNT" -gt 0 ]]; then
  printf '%s结论：未发现已执行路径中的 Sanitizer 错误，但仍有未覆盖或权限跳过的检查。%s 请按 WARN 处理后再作最终结论。\n' "$YELLOW$BOLD" "$RESET"
  exit 0
fi
printf '%s结论：本轮测试路径未发现 ASan/UBSan 报告的问题。%s 这不等同于“绝无内存泄漏”，还应扩充异常输入、长时间压测和并发场景。\n' "$GREEN$BOLD" "$RESET"
