# 第二轮验收报告：P3 Transport + P4 应用层安全通道

> 范围：执行规划 §22「第二轮工作与验收范围：P3 Transport + P4 安全通道」。
> 执行日期：2026-09-08
> 结论：**第二轮通过（P3/P4 门禁全绿）**，并在收口过程中发现并修复了两处此前被
> 掩盖的真实问题（见 §5）。完整业务 e2e 已解除 DISABLED，且在普通构建与
> ASan+UBSan 下均真实通过；security_e2e 已重写为可对当前握手模型生效的 fail-close
> 验证。

---

## 1. 收口前的真实状态（本轮开始时）

上一轮已经写完 P3/P4 的代码（`FrameCodec`、`ClientSecureChannel`、`test_frame_codec`、
`test_transport`、`test_secure_channel`，且 `test_e2e` 已去掉 DISABLED），但**尚未完成
测试与收口**：

- §22.5 要求的三端一致性向量 `client_core/tests/golden/app-security-v1.json` 缺失；
- §22.5 要求的 `outputs/kernel-round2-*.log` 证据一个都没有；
- 更严重：`test_e2e` / `test_security_e2e` 在 **Release** 下“通过”是**假绿**（见 §5.1）。

因此本轮的工作是：真正把测试跑起来、补齐证据、修掉被假绿掩盖的缺陷。

---

## 2. P3 验收：Transport 与消息边界

| 门禁项（§22.2.4） | 结果 | 证据 |
|---|---|---|
| FrameCodec 覆盖全部边界矩阵 | ✅ | `test_frame_codec` 通过 |
| 真实 loopback TLS 半包/粘包/1 字节分段/中途断开 | ✅ | `test_transport` 通过（`kernel-round2-transport-test.log`） |
| 连续重连 50 次无线程/句柄持续增长 | ✅ | `test_transport` 重连用例通过 |
| 单写队列并发压力无交叉写损坏 | ✅ | `test_transport` 发送队列并发用例通过 |
| 错误包长后安全断线并可新建连接 | ✅ | FrameCodec fail-close 用例通过 |
| client_core 普通 CTest 与 ASan/UBSan 全绿 | ✅ | 6/6（`kernel-round2-asan-client.log`） |

```text
client_core CTest（Release）：6/6 passed
  protocol / frame_codec / storage / integration / transport / secure_channel
client_core ASan+UBSan：6/6 passed，无 ASan/UBSan 报告
```

## 3. P4 验收：TLS 与应用层安全通道

| 门禁项（§22.3.5） | 结果 | 证据 |
|---|---|---|
| TLS 1.3 + 证书链/域名/SNI/pin + 负向 fail-close | ✅ | `test_transport` / `test_integration` / `test_secure_channel` |
| 四步握手字段/字节序对齐服务端 | ✅ | `test_e2e` 真实握手成功；`test_secure_channel` 负向用例 |
| 三端安全向量逐字节一致 | ✅ | `app-security-v1.json` + `test_secure_channel` 回归断言（见 §4） |
| 篡改/重放/方向混淆/握手超时 | ✅ | `test_secure_channel` 负向用例全通过 |
| 抓包只见 TLS records，业务明文不可见 | ✅ | `kernel-round2-packet-capture-note.md` |
| security_e2e 继续证明无降级 | ✅ | 重写后 fail-close（`kernel-round2-e2e-test.log`） |
| 删除旧 test_e2e 的 DISABLED，完整业务 e2e 通过 | ✅ | `test_e2e PASSED`（exit=0，asserts 生效） |
| 完整业务 e2e 在 ASan+UBSan 下通过 | ✅ | `kernel-round2-asan-server-e2e.log` |

## 4. 三端一致性 Golden（§22.3.4）

`client_core/tests/golden/app-security-v1.json` 现在是**可复现**的确定性向量：

- 固定 Ed25519 身份私钥种子、固定客户端 X25519 私钥/nonce/randomId，
  且**服务端** X25519 私钥/nonce/sessionId 也固定（此前服务端用 `RAND_bytes`，
  导致 golden 每次运行都不同，等于没有一致性意义）。
- `test_secure_channel` 连续两次运行输出的 GOLDEN 段 md5 一致；并新增回归断言，
  一旦密钥调度 / transcript / AAD 语义变化即失败。
- 向量含 transcript hash、双向 key、finished key、client verify_data，以及
  sequence=1 的 AAD/nonce 语义与实际 ciphertext/tag。

## 5. 收口中发现并修复的真实问题

### 5.1 im_server 测试假绿（根因）

`im_server` 的测试目标从未取消 `NDEBUG`，Release 构建把测试里的 `assert()` **整体
编译掉**，于是 `test_e2e` / `test_security_e2e` 在 Release 下“瞬间通过”却几乎没有
执行任何真实链路。这掩盖了下面 5.2 的功能缺陷。

- 修复：在 `im_server/CMakeLists.txt` 的测试块加 `-UNDEBUG`（MSVC 用 `/UNDEBUG`），
  与 `client_core` 保持一致。修复后 Release 也会真正执行断言。

### 5.2 ClientCore 缺少设备证明（P-256），无法登录

服务端在 `feat(auth): bind tokens to Keystore device signatures` 之后，密码登录**强制
要求** `device_public_key` + `device_signature`（P-256 / SHA256withECDSA，签名绑定本次
应用会话）。但 C++ `ClientCore::sendLogin` 从未同步实现，任何登录都会被服务端以
`LOGIN_INVALID`（“拒绝非法登录参数”）拒绝。此前因 5.1 的假绿而未被发现。

- 修复：
  - 新增 `client_core/src/transport/DeviceProof.{h,cpp}`：P-256 密钥生成、X.509 SPKI
    DER 公钥导出、DER 签名，以及与服务端 `deviceproof::message` **逐字节一致**的规范
    message 构造。
  - `ClientCore` 持有 `DeviceProofKey`，`sendLogin` 现在构造并携带设备公钥与签名，
    签名内容为 `operation="password-login" | appSessionId | deviceId |
    (tel"\0"sha256(pass)) | publicKey`。
  - 每个 `ClientCore` 实例默认分配**唯一 device_id**（`makeDefaultDeviceId`），避免多
    实例复用同一 device_id 但公钥不同时被服务端“device_id 已绑定其他设备密钥”拒绝。

### 5.3 security_e2e 前提失效，已重写

原 security_e2e 依赖“`ClientCore` 只做 TLS、connectToServer 返回 true、再发明文
Login”，但 P4 后 `connectToServer` 会强制完成应用握手，旧写法在真实断言下不再成立。

- 修复：重写为**原始 asio TLS 客户端**——只完成真实 TLS 1.3 握手，然后跳过应用握手
  直接发送明文 `LoginRq`，断言服务端在 `WAIT_CLIENT_HELLO` 状态识别出非法明文业务帧
  并 fail-close，且不回发任何业务数据。这才真正验证了“未握手明文业务被拒”的不变式。

---

## 6. 交付物

代码：

```text
client_core/tests/golden/app-security-v1.json   三端一致性向量（新增，可复现）
client_core/src/transport/DeviceProof.{h,cpp}    P-256 设备证明（新增）
client_core/src/ClientCore.cpp                    sendLogin 携带设备证明 + 唯一 deviceId
client_core/tests/test_secure_channel.cpp         确定性 golden + 回归断言
im_server/tests/test_security_e2e.cpp             重写为原始 TLS fail-close 验证
im_server/CMakeLists.txt                          测试块加 -UNDEBUG，修复假绿根因
scripts/native_sanitizer_test.sh                  纳入 P3/P4 与完整 e2e 的 ASan/UBSan
```

证据：

```text
outputs/kernel-round2-transport-test.log         frame_codec + transport（Release）
outputs/kernel-round2-security-test.log          secure_channel + GOLDEN 段
outputs/kernel-round2-e2e-test.log               完整业务 e2e + security_e2e（Release，asserts 生效）
outputs/kernel-round2-asan-server-e2e.log        e2e + security_e2e（ASan+UBSan）
outputs/kernel-round2-asan-client.log            client_core 全套（ASan+UBSan）
outputs/kernel-round2-packet-capture-note.md     抓包与明文可见性说明
```

## 7. 已知遗留（不阻塞 P3/P4，按规划继续记录）

| 项 | 状态 | 说明 |
|---|---|---|
| x86_64 实跑 | ⏸ 延期 | 按 §22.1.3 本轮只要求 x86_64 编译通过 |
| Android arm64 真实安全链路 | ✅ 已完成 | arm64-v8a AVD 实跑真实 socket：TLS 1.3 + CA/hostname/SPKI → 四步应用握手 → 带 marker 的 AES-GCM Heartbeat → 解密响应，见 `kernel-round2-android-socket-e2e-final.log` |
| Android native 登录业务迁移 | ⏭ P5 | P4 已用 test-only JNI 关闭真实加密业务帧门禁；正式 UI connect/login/token refresh API 仍按规划进入 P5 |
| im_server DB 层单测 | ✅ 已排查，非代码缺陷 | 早前 `database_concurrency/token_service/device_proof/db_write_queue/conversation_migration` 在一次批量运行中崩溃（`out of memory`/`DbWriteQueue not running`/segfault），经复查为**环境瞬时状态**（紧邻一次被中断的 ASan e2e 之后，`/tmp` 残留/内存压力所致）：干净重建后逐个、整批、连跑两遍、以及 ASan+UBSan 下均 **13/13 全绿**。各测试各自使用独立 `/tmp` 路径且清理 `.db/-wal/-shm`，不存在相互干扰或代码 bug。证据见 `kernel-round2-asan-full.log` |
| AVIF 偏色、Media Golden | ⚠️ 预存 | 沿用第一轮记录，不在 P3/P4 范围 |
| ClientCore Token 登录/刷新 | ⏭ P5 | 本轮只做密码登录设备证明；TokenLogin/Refresh 的设备签名留待 P5 |

## 8. 判定

按 §22.6：

- P3、P4 所有门禁项完成，没有跳过的安全负向测试；
- 旧完整业务 e2e 已解除 DISABLED，普通与 ASan+UBSan 均**真实**通过（非假绿）；
- 错误包长、错误证书、错误签名、篡改密文、重放 sequence、未握手明文业务均 fail-close；
- 没有 verify_none、明文业务降级、并发写、未认证明文上抛等临时兼容代码；
- 第一轮 Native 生命周期 / CheckJNI / HWASan / Search Golden 未在本轮改动，不受影响。

F05 已在 `emulator-5554`（Android 14 / arm64-v8a）完成真实网络闭环。F06 同次运行在
AVD 内实际抓取 52 个包；32 字节唯一 marker
`JITONG_F06_20260908_FINAL_B93D2E` 未出现在 pcap，文件 SHA-256 为
`a60f9f3eb2dde3a2f8e5b60cdb906ec4575e72d720edadbe5d0fd0761ba613ad`。

**最终且唯一结论：第二轮 P3/P4 验收完成。** 正式 Native 登录/Token API 和设备身份持久化
属于 P5，已在上表中明确顺延，不与本轮结论冲突。
