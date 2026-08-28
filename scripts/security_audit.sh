#!/usr/bin/env bash
# 静态安全基线审计：面向本项目的 Android 客户端 + C++ IM 服务端。
#
# 默认只读扫描源码，不连接网络、不启动服务、不发送攻击流量。
#   bash scripts/security_audit.sh
# 可选执行本地构建与测试（会创建/更新 build-security-audit 目录）：
#   bash scripts/security_audit.sh --run-build
# 可选执行服务端回环集成测试（会绑定本机 24563 端口，请先关闭 im_server）：
#   bash scripts/security_audit.sh --run-integration
#
# 这是“生产级安全差距”审计，而非渗透测试报告或安全认证。FAIL 表示不能按
# 生产级标准上线；PASS 仅说明此项静态证据存在，仍需人工代码审计、依赖漏洞
# 扫描、渗透测试、日志审计与真实环境配置核验。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_BUILD=0
RUN_INTEGRATION=0
PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

if [[ -t 1 ]]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BLUE=$'\033[34m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; BLUE=''; BOLD=''; RESET=''
fi

usage() {
  sed -n '1,13p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
  case "$arg" in
    --run-build) RUN_BUILD=1 ;;
    --run-integration) RUN_INTEGRATION=1 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "未知参数：$arg（可用 --help 查看）" >&2; exit 2 ;;
  esac
done

pass() { printf '%s[PASS]%s %s\n' "$GREEN$BOLD" "$RESET" "$*"; PASS_COUNT=$((PASS_COUNT + 1)); }
warn() { printf '%s[WARN]%s %s\n' "$YELLOW$BOLD" "$RESET" "$*"; WARN_COUNT=$((WARN_COUNT + 1)); }
fail() { printf '%s[FAIL]%s %s\n' "$RED$BOLD" "$RESET" "$*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
info() { printf '%s[INFO]%s %s\n' "$BLUE" "$RESET" "$*"; }
section() { printf '\n%s%s%s\n' "$BOLD" "$*" "$RESET"; }

# 所有正则扫描均跳过构建物、第三方实现和 protobuf 生成物，避免把依赖代码当成项目结论。
SOURCE_GLOBS=(
  --glob '!**/build/**' --glob '!**/build-*/**' --glob '!**/.gradle/**'
  --glob '!client_core/third_party/**' --glob '!protocol/generated/**'
  --glob '!IMClient/**' --glob '!IMServer/**'
)
HAS_RG=0
command -v rg >/dev/null 2>&1 && HAS_RG=1

# 某些精简环境没有 ripgrep；自动回退到 macOS/Linux 自带 grep，保证“找不到 rg”
# 不会被误判为“源码中不存在防护”。
source_file_list() {
  find "$ROOT/im_server" "$ROOT/client_core" "$ROOT/protocol" "$ROOT/jitong_android" \
    \( -path '*/build/*' -o -path '*/build-*/*' -o -path '*/.gradle/*' -o \
       -path '*/third_party/*' -o -path '*/generated/*' \) -prune -o \
    -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o \
              -name '*.hpp' -o -name '*.kt' -o -name '*.kts' -o -name '*.xml' -o \
              -name '*.proto' \) -print0 2>/dev/null
}
source_has() {
  local pattern="$1" file
  if [[ "$HAS_RG" -eq 1 ]]; then
    rg -q -e "$pattern" "$ROOT/im_server" "$ROOT/client_core" "$ROOT/protocol" "$ROOT/jitong_android" "${SOURCE_GLOBS[@]}" 2>/dev/null
    return
  fi
  while IFS= read -r -d '' file; do
    grep -E -q -e "$pattern" "$file" 2>/dev/null && return 0
  done < <(source_file_list)
  return 1
}
source_files() {
  local pattern="$1" file
  if [[ "$HAS_RG" -eq 1 ]]; then
    rg -l -e "$pattern" "$ROOT/im_server" "$ROOT/client_core" "$ROOT/protocol" "$ROOT/jitong_android" "${SOURCE_GLOBS[@]}" 2>/dev/null || true
    return
  fi
  while IFS= read -r -d '' file; do
    grep -E -q -e "$pattern" "$file" 2>/dev/null && printf '%s\n' "$file"
  done < <(source_file_list)
}
file_has() {
  local pattern="$1" file="$2"
  if [[ "$HAS_RG" -eq 1 ]]; then rg -q -e "$pattern" "$file" 2>/dev/null; else grep -E -q -e "$pattern" "$file" 2>/dev/null; fi
}
repo_match_files() {
  local pattern="$1" file
  if [[ "$HAS_RG" -eq 1 ]]; then
    rg -l -i -e "$pattern" "$ROOT" --glob '!**/build/**' --glob '!**/build-*/**' --glob '!**/.gradle/**' \
      --glob '!**/third_party/**' --glob '!**/generated/**' --glob '!**/uploads/**' --glob '!**/dist/**' \
      --glob '!**/node_modules/**' --glob '!**/.next/**' --glob '!scripts/security_audit.sh' 2>/dev/null || true
    return
  fi
  while IFS= read -r -d '' file; do
    grep -E -i -q -e "$pattern" "$file" 2>/dev/null && printf '%s\n' "$file"
  done < <(find "$ROOT" \( -path '*/.git/*' -o -path '*/build/*' -o -path '*/build-*/*' -o \
      -path '*/.gradle/*' -o -path '*/third_party/*' -o -path '*/generated/*' -o \
      -path '*/node_modules/*' -o -path '*/.next/*' -o -path '*/uploads/*' -o -path '*/dist/*' \) -prune -o \
      -type f ! -path '*/scripts/security_audit.sh' -print0 2>/dev/null)
}

section 'Jitong IM 生产级安全基线审计'
info "仓库：$ROOT"
info "时间：$(date '+%Y-%m-%d %H:%M:%S %Z')"
info '范围：Android 客户端、client_core、protocol、im_server；不包含旧桌面端 IMClient/IMServer。'

section '1. 传输与登录凭据'
MANIFEST="$ROOT/jitong_android/app/src/main/AndroidManifest.xml"
if file_has 'usesCleartextTraffic="true"' "$MANIFEST"; then
  fail 'Android 已允许明文 HTTP/TCP（usesCleartextTraffic=true）：局域网嗅探、篡改与中间人攻击均可发生。生产必须改为 TLS 1.2+，并关闭明文传输。'
else
  pass 'Android Manifest 未显式允许明文流量。'
fi

if source_has '(asio::ssl|ssl::context|SSL_CTX|mbedTLS|rustls|OpenSSL)'; then
  pass '源码中发现 TLS/SSL 相关实现；仍需人工核验客户端证书校验、最低 TLS 版本和证书链。'
else
  fail '未发现应用层 TLS/SSL 实现。当前自定义 TCP 协议不能达到生产级传输保密性与抗中间人要求。'
fi

if source_has 'sha256Hex\(pass\)|sha256\(salt \+ passHash\)|sha256\(明文\)'; then
  fail '登录密码使用快速 SHA-256 派生；且 SHA-256(密码) 会上链路，存在离线爆破与重放风险。生产应使用 TLS + 服务端 Argon2id/PBKDF2/scrypt（独立随机盐、足够成本参数）。'
else
  pass '未扫描到当前已知的快速 SHA-256 密码派生模式。'
fi

if source_has '(rate.?limit|throttl|too_many|LOGIN_TOO)'; then
  pass '发现登录/接口限速相关实现；请人工验证按账号、IP、设备的阈值与锁定策略。'
else
  fail '未发现登录失败限速、退避或账户锁定逻辑。生产环境容易遭受撞库和暴力破解。'
fi

if source_has '(token|jwt|session.*expir|refresh.?token)'; then
  warn '发现令牌/会话相关关键词；请核验会话过期、撤销、重新认证与断线重连后的身份绑定。'
else
  warn '未发现独立令牌及过期机制。长连接可依赖连接态鉴权，但生产仍应设计会话生命周期、主动失效与设备管理。'
fi

section '2. 输入、协议与文件边界'
if source_has 'MAX_PACK_LEN.*10.*1024.*1024|len.*MAX_PACK_LEN|bodyLen.*MAX_PACK_LEN'; then
  pass '发现协议帧长度上限校验（10 MiB）及接收端拒绝逻辑。'
else
  fail '未能确认协议帧长度上限校验；可能被超长帧消耗内存。'
fi

if source_has 'FILE_MAX_SIZE.*100|file_size\(\).*FILE_MAX_SIZE'; then
  pass '发现文件大小上限校验（100 MiB）。'
else
  fail '未能确认文件大小上限校验。'
fi

if source_has 'isSafeFileId' && source_has '(sha256|Sha256)'; then
  pass '发现文件 ID 安全校验与完整性 SHA-256 校验。'
else
  fail '未同时确认文件 ID 防路径穿越校验和文件完整性校验。'
fi

DANGEROUS_CALLS="$(source_files '(system|popen|gets|strcpy|strcat|sprintf)[[:space:]]*\(')"
if [[ -n "$DANGEROUS_CALLS" ]]; then
  warn "发现需人工复核的危险 API：$(printf '%s' "$DANGEROUS_CALLS" | tr '\n' ' ')"
else
  pass '未发现 system/popen/gets/strcpy/strcat/sprintf 等高风险 C/C++ API。'
fi

if source_has '(total_chunks\(\)|totalChunks).*([<>=]|MAX|0)'; then
  pass '发现分片数量相关边界处理；建议人工进一步验证负数、超大数量、重复块和乱序块。'
else
  warn '未静态识别到明确的分片数量边界检查；建议补充恶意分片与重复提交测试。'
fi

section '3. 数据与 Android 发布配置'
if source_has 'sqlite3_prepare_v2' && source_has 'sqlite3_bind_(int|int64|text|blob)'; then
  pass 'SQLite 已使用预编译与参数绑定；仍需逐条人工确认所有用户输入均经 bind。'
else
  fail '未确认 SQLite 预编译与参数绑定。'
fi

if source_has '(SQLCipher|SupportFactory|EncryptedSharedPreferences|MasterKey)'; then
  pass '发现本地敏感数据加密组件；请人工核验密钥存储与迁移。'
else
  fail '未发现 Android 本地数据库/偏好设置的加密组件。生产环境应采用 SQLCipher 或等效方案，并用 Android Keystore 保护密钥。'
fi

if file_has 'android:allowBackup="false"' "$MANIFEST"; then
  pass 'Android 已禁止应用数据备份。'
else
  warn 'Manifest 未设置 android:allowBackup="false"；默认备份策略可能导出聊天数据库或偏好设置。'
fi

if file_has 'android:debuggable="true"' "$MANIFEST" || source_has 'debuggable[[:space:]]*=[[:space:]]*true'; then
  fail '发现可发布配置可能启用 debuggable=true。生产 APK 必须使用非调试签名与 release 构建。'
else
  pass '源码中未发现显式 debuggable=true；仍需检查最终 release APK 的 manifest。'
fi

FILE_PROVIDER="$ROOT/jitong_android/app/src/main/res/xml/file_paths.xml"
if [[ -f "$FILE_PROVIDER" ]] && ! file_has '<root-path|path="/"' "$FILE_PROVIDER"; then
  pass 'FileProvider 路径配置未开放 root-path 或根目录。'
else
  warn '未能确认 FileProvider 路径是否最小授权，请检查 res/xml/file_paths.xml。'
fi

section '4. 密钥、依赖与可审计性'
SECRET_FILES="$(repo_match_files '(-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----|api[_-]?key[[:space:]]*[:=]|secret[[:space:]]*[:=]|password[[:space:]]*[:=])')"
if [[ -n "$SECRET_FILES" ]]; then
  warn "发现疑似硬编码凭据的候选文件（请人工确认，脚本不打印内容）：$(printf '%s' "$SECRET_FILES" | tr '\n' ' ')"
else
  pass '未发现常见私钥/API key/secret/password 硬编码模式。'
fi

if [[ -f "$ROOT/jitong_android/gradlew" ]]; then
  warn 'Android 依赖漏洞扫描需在具备 Android SDK/JDK 的环境执行；建议另跑：./gradlew dependencyCheckAnalyze 或接入 Dependabot/OSV。'
else
  warn '未找到 Android Gradle Wrapper，无法在此脚本内执行 Android 依赖锁定/漏洞扫描。'
fi

if [[ -f "$ROOT/.gitignore" ]] && file_has '(\.db|uploads|\.env|keystore)' "$ROOT/.gitignore"; then
  pass '发现 .gitignore 对部分运行时敏感文件的忽略规则。'
else
  warn '未确认 .gitignore 已覆盖数据库、上传文件、.env 和签名密钥。'
fi

if [[ "$RUN_BUILD" -eq 1 ]]; then
  section '5. 可选：client_core 构建与单元测试'
  AUDIT_BUILD="$ROOT/client_core/build-security-audit"
  info "构建目录：$AUDIT_BUILD"
  if cmake -S "$ROOT/client_core" -B "$AUDIT_BUILD" -DCLIENT_CORE_BUILD_TESTS=ON -DCLIENT_CORE_BUILD_TOOLS=OFF && cmake --build "$AUDIT_BUILD" --parallel; then
    pass 'client_core 构建成功。'
    if ctest --test-dir "$AUDIT_BUILD" --output-on-failure; then
      pass 'client_core 协议、存储与回环集成测试通过。'
    else
      fail 'client_core 测试失败；请保留失败输出并修复后再评估。'
    fi
  else
    fail 'client_core 构建失败；无法完成基础安全回归。'
  fi
else
  info '跳过构建/单元测试（加 --run-build 执行）。'
fi

if [[ "$RUN_INTEGRATION" -eq 1 ]]; then
  section '6. 可选：服务端端到端边界回归'
  SERVER_BUILD="$ROOT/im_server/build"
  if [[ ! -f "$SERVER_BUILD/CTestTestfile.cmake" ]]; then
    info '未发现 im_server/build，将先配置并构建。'
    cmake -S "$ROOT/im_server" -B "$SERVER_BUILD" -DIM_SERVER_BUILD_TESTS=ON
  fi
  if cmake --build "$SERVER_BUILD" --parallel && ctest --test-dir "$SERVER_BUILD" -R '^e2e$' --output-on-failure; then
    pass '服务端 e2e 测试通过（含文件大小/哈希边界回归）。'
  else
    fail '服务端 e2e 测试失败。请先确保 24563 未被 im_server 占用，再检查失败日志。'
  fi
else
  info '跳过服务端 e2e（加 --run-integration 执行；它会绑定 24563 端口）。'
fi

section '审计结论'
printf 'PASS=%s  WARN=%s  FAIL=%s\n' "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"
if [[ "$FAIL_COUNT" -gt 0 ]]; then
  printf '%s生产级结论：不通过。%s 当前项目具备若干基础防护，但 TLS、密码 KDF/抗重放、登录限流、端侧加密等 FAIL 项完成前，不应承载真实用户数据或暴露到公网。\n' "$RED$BOLD" "$RESET"
  exit 1
fi
printf '%s生产级结论：静态基线未发现阻塞项。%s 仍需执行动态渗透测试、SAST/SCA、Android release APK 检查和部署配置评审。\n' "$GREEN$BOLD" "$RESET"
