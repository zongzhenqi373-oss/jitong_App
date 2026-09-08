# 第二轮抓包与明文可见性说明（P4 门禁）

> 目标：证明在真实链路上，业务正文既被 TLS 1.3 record 加密，又被应用层
> AES-256-GCM 二次加密（AppEncryptedFrame），抓包中看不到任何业务明文、测试关键词
> 或可解析的 protobuf 业务包。

## 1. 结论

- 业务帧在 TLS 之上再套一层应用层安全通道（X25519 + Ed25519 + HKDF-SHA256 +
  AES-256-GCM）。握手完成后，所有业务（登录、消息、漫游、文件卡片）都以
  `AppEncryptedFrame(1040)` 承载，明文 protocol_type 与 payload 均在 GCM 密文内。
- 服务端日志中的 `[APP-SEC] 已认证解密业务帧 seq=N innerType=... payloadBytes=...`
  即证明：外层只能看到 1040 与密文长度，真正的 innerType（如 1000/1002/1005/1010）
  只有解密后才可见。
- 未完成应用握手就发送明文业务帧时，服务端 fail-close（见
  `kernel-round2-e2e-test.log` 末尾的 security_e2e 段：
  `非法握手帧 state=WAIT_CLIENT_HELLO actualType=1002 ... 握手失败`）。

### 1.1 2026-09-08 实际抓包结果

本次不是用服务端日志替代抓包。由于 macOS BPF 需要管理员口令，改在已 root 的 arm64
AVD 内使用系统 `tcpdump`，对 `any` 接口且仅过滤 `tcp port 24680`：

```text
设备：emulator-5554 / Android 14 / arm64-v8a
客户端：10.0.2.16:39738
服务端：10.0.2.2:24680
包数：52
pcap：outputs/kernel-round2-android-arm64.pcap
SHA-256：a60f9f3eb2dde3a2f8e5b60cdb906ec4575e72d720edadbe5d0fd0761ba613ad
唯一测试 marker：JITONG_F06_20260908_FINAL_B93D2E
marker 搜索结果：MARKER_NOT_FOUND
```

同一次测试的 androidTest 输出为 `tls_and_app_handshake=ok`、
`marker_probe_enqueued=ok`、`encrypted_heartbeat_roundtrip=ok`；服务端解密后确认第一条
Heartbeat 的 payload 为 32 字节。也就是说 marker 确实进入了业务明文并发上线路，但在
pcap 中不可见，不是“没有发送 marker 所以搜不到”的假证据。

## 2. 如何本机复现抓包

在回环上抓 e2e 端口（服务端 24680，HTTP 文件 24681）：

```bash
# 终端 A：抓包（需要 sudo；-A 打印可见 ASCII 便于人工搜关键词）
sudo tcpdump -i lo0 -A -s 0 'tcp port 24680' -w /tmp/round2_loopback.pcap

# 终端 B：跑完整业务 e2e
./build/im-server/test_e2e

# 停止抓包后离线检查：不应出现任何业务明文/测试关键词
strings /tmp/round2_loopback.pcap | grep -E '张三|李四|你好李四|这是离线消息|13800000001' || echo "no plaintext business found"
```

预期：

- `grep` 无输出（打印 `no plaintext business found`）。测试里的中文昵称、消息正文、
  手机号都不会出现在密文流量中。
- 握手前的 TLS 记录只暴露证书链等 TLS 元数据；握手后全部是 application_data record。

## 3. 双层加密与 fail-close 的自动化取证

无需人工抓包也可得到等价证据的自动化测试：

| 证据 | 文件 | 说明 |
|---|---|---|
| 业务只走 1040 密文 | `kernel-round2-e2e-test.log` | 全程 `已认证解密业务帧`，innerType 只在解密后出现 |
| 明文业务被 fail-close | `kernel-round2-e2e-test.log`（security_e2e 段） | 明文 Login(1002) 在 WAIT_CLIENT_HELLO 被拒并断开 |
| 三端安全向量一致 | `kernel-round2-security-test.log` + `client_core/tests/golden/app-security-v1.json` | 确定性 transcript/key/finished/加密帧 |
| ASan/UBSan 无内存问题 | `kernel-round2-asan-server-e2e.log`、`kernel-round2-asan-client.log` | e2e/security_e2e/transport/secure_channel 全绿 |

## 4. 说明

- 本机为 macOS，回环设备名为 `lo0`（Linux 为 `lo`）。抓包命令按平台替换网卡名。
- 本轮 pcap 已保存为 `outputs/kernel-round2-android-arm64.pcap` 供本地复核。pcap 含连接
  时间、地址和端口元数据；推送公开仓库前应按项目的数据分级规则决定是否纳入版本库。
