#!/usr/bin/env bash
set -euo pipefail

# 仅用于抓取本机即通开发环境流量：Android 模拟器 -> 宿主机 im_server。
# 用法：sudo ./scripts/capture_im_security.sh [抓取秒数] [测试明文关键词]
# 示例：sudo ./scripts/capture_im_security.sh 20 JITONG_SECRET_829

DURATION="${1:-20}"
MARKER="${2:-}"
PORT="${JITONG_CAPTURE_PORT:-24563}"
INTERFACE="${JITONG_CAPTURE_INTERFACE:-lo0}"
OUTPUT_DIR="${JITONG_CAPTURE_OUTPUT_DIR:-./tmp/security-capture}"
PCAP_FILE="${OUTPUT_DIR}/jitong-${PORT}-$(date +%Y%m%d-%H%M%S).pcap"

if ! [[ "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
  echo "错误：抓取秒数必须是正整数" >&2
  exit 2
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "错误：没有找到 tcpdump" >&2
  exit 3
fi

mkdir -p "$OUTPUT_DIR"

echo "[1/4] 开始抓取 ${INTERFACE} 上 TCP ${PORT}，持续 ${DURATION} 秒"
echo "      现在请在 App 中登录，并发送一条包含测试关键词的消息。"
echo "      输出文件：${PCAP_FILE}"

tcpdump -i "$INTERFACE" -nn -s 0 -U -w "$PCAP_FILE" "tcp port ${PORT}" &
TCPDUMP_PID=$!

cleanup() {
  if kill -0 "$TCPDUMP_PID" >/dev/null 2>&1; then
    kill -INT "$TCPDUMP_PID" >/dev/null 2>&1 || true
    wait "$TCPDUMP_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

sleep "$DURATION"
cleanup
trap - EXIT INT TERM

echo
echo "[2/4] 数据包摘要（只能看到地址、端口、长度和 TLS 记录）"
tcpdump -nn -tttt -r "$PCAP_FILE" 2>/dev/null | head -40 || true

echo
echo "[3/4] TCP 负载十六进制预览（内容应表现为 TLS 密文）"
tcpdump -nn -X -c 6 -r "$PCAP_FILE" 2>/dev/null || true

echo
echo "[4/4] 明文泄漏检查"
if [[ -n "$MARKER" ]]; then
  if LC_ALL=C grep -aF -- "$MARKER" "$PCAP_FILE" >/dev/null; then
    echo "[危险] 在抓包文件中发现测试关键词：${MARKER}"
    exit 10
  fi
  echo "[通过] 抓包文件中未发现测试关键词：${MARKER}"
else
  echo "未提供测试关键词，只列出可打印字符串。建议重新运行并传入唯一关键词。"
fi

echo
echo "可打印字符串预览："
strings "$PCAP_FILE" | head -40 || true

echo
echo "抓包完成：${PCAP_FILE}"
echo "Wireshark 过滤条件：tcp.port == ${PORT}"
echo "预期：可识别 TLS 1.3 流量，但看不到账号、密码摘要、Token、真实协议号或聊天正文。"
