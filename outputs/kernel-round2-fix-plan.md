# 第二轮代码审查问题修复方案

> 对应范围：P3 Transport + P4 TLS / 应用层安全通道
>
> 输入材料：`kernel-round2-acceptance.md`、`kernel-round2-packet-capture-note.md`、`kernel-round2-android-arm64-note.md`
>
> 最终结论（2026-09-08）：**F01～F07 已关闭，第二轮 P3/P4 验收完成。**
>
> 本文只定义修复任务与验收方法，不代表代码已经修改。

---

## 1. 修复目标

本次收口不是推翻现有 P3/P4，而是在已有实现上关闭以下风险：

1. 将应用层加密、sequence 分配和发送入队串行化，避免 AES-GCM nonce 重复及乱序。
2. 把 SPKI Pinning 真正接入 `ClientCore` 配置，并修正多级证书链的 pin 校验位置。
3. 补齐真实的多线程、万帧单写压力测试。
4. 将 Golden 从“C++ 自洽测试”升级为 Android/C++/Server 独立实现共同消费的向量。
5. 在 Android arm64 上完成真实 TLS + 应用握手 + 加密业务帧端到端测试。
6. 实际执行抓包并保存可复核证据。
7. 修正 Golden 协议描述及第二轮验收报告的矛盾结论。
8. 明确设备身份持久化属于 P5，在第二轮报告中如实标注当前限制。

---

## 2. 优先级与执行顺序

```text
R2-F01 并发加密与发送串行化（P0）
  ↓
R2-F02 SPKI Pinning 接入和证书链修复（P0）
  ↓
R2-F03 Transport/加密并发压力测试（P1）
  ↓
R2-F04 三端 Golden 独立验证（P1）
  ↓
R2-F05 Android arm64 真实端到端链路（P1）
  ↓
R2-F06 真实抓包取证（P1）
  ↓
R2-F07 报告统一与最终复验（P1）
```

F01、F02 是代码正确性和安全性问题，必须先修；F03～F06 是门禁证据补齐；F07 只能在前面的测试全部结束后执行。

---

## 3. R2-F01：加密、sequence 与发送入队串行化

### 3.1 问题

`ClientCore::sendPacket()` 可能被 UI、心跳线程和业务工作线程并发调用。当前代码在进入 `TcpTransport::send()` 的单写队列之前就调用 `ClientSecureChannel::encrypt()`，而 `encrypt()` 会无锁递增 `m_sendSequence`。

风险包括：

- C++ 数据竞争和未定义行为；
- 两个帧得到重复 sequence；
- AES-GCM 在同一 key 下重复 nonce；
- sequence 分配顺序与 Transport 入队顺序不一致；
- 服务端因 sequence 跳号/回退而 fail-close。

### 3.2 推荐方案

不要只把 `m_sendSequence` 改成 `atomic`，也不要只在 `encrypt()` 上加互斥锁。正确边界应为：

```text
任意业务线程调用 sendPacket
→ 复制 type + payload，post 到 Transport/Connection 的统一 strand
→ strand 内检查连接 generation 和 SecureChannel 状态
→ strand 内分配 sequence
→ strand 内 AES-GCM 加密
→ strand 内组装外层 1040 帧
→ 直接进入同一 executor 的单写队列
```

这样才能同时保证：

```text
sequence 顺序 = 加密顺序 = 入队顺序 = TLS stream 写顺序
```

### 3.3 建议修改文件

- `client_core/src/ClientCore.cpp`
- `client_core/include/client_core/ClientCore.h`
- `client_core/src/TcpTransport.h`
- `client_core/src/TcpTransport.cpp`
- 可选新增：`client_core/src/transport/ConnectionExecutor.{h,cpp}`

### 3.4 接口建议

方案 A（推荐）：由 `TcpTransport` 暴露串行执行入口，`ClientCore` 将加密闭包交给它。

```cpp
using SerializedTask = std::function<void()>;
void postSerialized(SerializedTask task);
```

更完整的设计是把 SecureChannel 下沉到 `ConnectionManager`，提供：

```cpp
void sendBusiness(proto::protType type,
                  std::string payload,
                  SendCallback callback);
```

由 ConnectionManager 在自己的 strand 中完成加密和写队列调度，UI/业务层不能直接操作 SecureChannel。

### 3.5 失败语义

每个发送请求必须恰好完成一次：

- 未连接：`NotConnected`；
- 握手未完成：`NotAuthenticated`；
- 加密失败：`CryptoError`，并关闭当前安全会话；
- close 前尚未处理：`Cancelled`；
- TLS 写失败：`Disconnected`。

禁止当前 `ClientCore::sendRawPacket()` 忽略 `TcpTransport::send()` 返回值的行为继续扩散。

### 3.6 验收测试

- 8 个线程同时发送 10,000 个加密业务帧。
- 服务端解密得到 sequence `1..10000`，无重复、跳号和回退。
- 每个 payload 都携带唯一编号，服务端最终集合与发送集合完全一致。
- 同时运行心跳与业务发送，心跳不导致业务 sequence 乱序。
- 并发发送过程中 close，10,000 个请求每个恰好收到一次完成回调。
- 普通、ASan+UBSan 通过；支持的平台增加 TSan。

门禁：出现任何重复 nonce、sequence 缺口、重复回调、悬挂回调或数据竞争即失败。

---

## 4. R2-F02：SPKI Pinning 真正接入并修正证书链逻辑

### 4.1 问题一：ClientCore 无法配置 pin

`TcpTransport` 有 `setSpkiPins()`，但 `ClientConfig` 没有 pins 字段，`ClientCore` 构造后也没有调用该方法。因此真实客户端从未启用 SPKI Pinning。

### 4.2 修改方案

在 `ClientConfig` 增加：

```cpp
std::vector<std::string> spkiPins;
bool requireSpkiPinning = true;
```

生产构建规则：

- `requireSpkiPinning=true` 且 pins 为空：创建或连接失败；
- 至少支持 current/next 两个 pin，便于证书轮换；
- 测试如确需只验证 CA，可显式使用测试配置，但不能存在生产环境静默降级；
- Android JNI 配置层必须把 pins 传到 `ClientConfig`，不能只存在 Kotlin 常量中。

建议修改：

- `client_core/include/client_core/ClientCore.h`
- `client_core/src/ClientCore.cpp`
- `client_core/src/TcpTransport.h`
- `client_core/src/TcpTransport.cpp`
- `jitong_android/app/src/main/cpp/native_sdk_handle.{h,cpp}`
- `jitong_android/app/src/main/cpp/im_core_jni.cpp`
- `jitong_android/app/src/main/java/com/jitong/im/core/NativeBindings.kt`

### 4.3 问题二：pin 被错误应用到证书链每一层

OpenSSL verify callback 会按证书深度多次调用。现实现对每次 `current_cert` 都要求命中同一组 pins，叶子 pin 可能在验证中间证书时被拒绝。

修正逻辑：

```cpp
if (!preverified) return false;

const int depth = X509_STORE_CTX_get_error_depth(ctx.native_handle());
if (depth != 0) {
    return true; // 中间证书/根证书只沿用 OpenSSL 链验证结果
}

// depth == 0：叶子证书
// 1. hostname 校验
// 2. SPKI SHA-256 与 current/next pins 做常量时间比较
```

建议优先使用 OpenSSL 已完成链验证后的 leaf certificate，避免在 callback 每层重复执行 hostname 验证。

### 4.4 验收矩阵

| 场景 | 预期 |
|---|---|
| 受信链 + hostname 正确 + current pin | 成功 |
| 受信链 + hostname 正确 + next pin | 成功 |
| 受信链 + hostname 正确 + pin 不匹配 | 握手失败 |
| 受信链 + pins 为空且 require=true | 握手前失败 |
| 受信链 + SAN 错误 | 失败 |
| 自签但未被 CA 信任 | 失败 |
| 过期证书 | 失败 |
| 真实三层证书链 + 叶子 pin | 成功，不要求中间证书命中叶子 pin |
| pin 失败 | 不得发送 AppClientHello |

门禁：测试代码和生产代码中不得出现 `verify_none`、永远返回 true 的 callback 或 pin 失败后继续连接。

---

## 5. R2-F03：补齐 P3/P4 并发与生命周期压力

### 5.1 Transport 万帧测试

当前 32 帧 close 测试只能验证取消回调，不能替代并发单写压力。

新增测试：

```text
8 sender threads × 1250 frames
→ Transport 串行写
→ TLS loopback server 用 FrameReader 解帧
→ 校验总数 10000
→ 校验每个唯一编号只出现一次
→ 校验帧边界和 payload 完整
```

测试服务端不能只统计 `bytesReceived`，必须真正解帧并校验内容。

### 5.2 生命周期测试

补充：

- DNS resolve 期间 close；
- TCP connect 期间 close；
- TLS handshake 期间 close；
- packet callback 内调用 close；
- close callback 内销毁上层对象；
- 连接超时后立即重连；
- 旧 generation 的 resolver/connect/handshake/write/read 回调迟到；
- 反复创建、连接、断开、销毁 100 次。

重点验证 `TcpTransport::stopIoThread()` 的 IO 线程 detach 分支不会留下捕获 `this` 的回调，也不会在对象析构后继续访问成员。

### 5.3 预期修改

- `client_core/tests/test_transport.cpp`
- `client_core/tests/test_integration.cpp`
- `client_core/tests/test_secure_channel.cpp`
- 视测试结果修正 `TcpTransport.{h,cpp}`

---

## 6. R2-F04：建立真正的三端 Golden

### 6.1 先修正向量说明

`app-security-v1.json` 必须与真实代码一致：

```text
salt = SHA256(client_nonce || server_nonce)
info = "jitong-app-channel-v1" || session_id || transcript_hash
```

不得继续保留错误的 `jitong-app-keys-v1` 或未哈希 salt 描述。

### 6.2 三端独立消费

同一份 JSON 分别由三套生产实现读取并计算：

1. C++：`ClientSecureChannel` Golden Test。
2. Android：Kotlin `SecureChannel.kt` 的 Golden Test；如果 Kotlin 实现即将废弃，也必须作为现状兼容基线运行一次并保存结果。
3. Server：直接调用 `AppCrypto`/服务端安全通道生产函数的 Golden Test，不能在测试文件复制一套算法。

每端必须独立断言：

- Ed25519 identity public key；
- X25519 public/shared secret；
- signing transcript 与 transcript hash；
- HKDF 双向 keys、nonce prefixes、Finished keys；
- client/server Finished；
- sequence=1 的 nonce、AAD、ciphertext、tag。

### 6.3 防止“共同写错”

- 测试不得从被测对象直接读取 key 后代替服务端计算 Finished；
- FakeServer 不得复制生产算法作为唯一真值；
- JSON 是固定外部事实源，只有协议版本正式升级时才允许更新；
- 更新向量必须经过 ADR，并同时说明向后兼容策略。

---

## 7. R2-F05：Android arm64 真实端到端链路

### 7.1 验收目标

必须在 Android App 进程中实际完成：

```text
NativeBindings.connect(config)
→ 10.0.2.2:24680
→ TLS 1.3 + CA/hostname/SPKI
→ AppClientHello / AppServerHello / 双 Finished
→ AppEncryptedFrame(HeartbeatRq)
→ 服务端解密并回复 HeartbeatRs
→ Android Native 解密响应
```

密码登录是否纳入取决于 P5 边界；但 P4 至少应完成一个真实加密 Heartbeat 请求与响应，不能仅做裸 TCP reachability。

### 7.2 最小 JNI 测试接口

可以只新增 test-only/instrumentation 入口，不提前迁移 P5 业务：

```text
nativeConnectForTest(config)
nativeEncryptedHeartbeatForTest()
nativeDisconnectForTest()
```

这些入口只用于验证 P3/P4，不包含 Token 自动刷新或登录状态机。

### 7.3 验收证据

- 设备 ABI、Android 版本、APK commit；
- 客户端 TLS 版本、证书验证、pin 命中记录；
- 服务端 App sessionId 的脱敏摘要；
- 客户端发送和服务端接收的 sequence；
- HeartbeatRs 解密成功；
- CheckJNI 和 HWASan 无错误。

---

## 8. R2-F06：执行真实抓包取证

### 8.1 执行要求

实际运行 `tcpdump` 或 Wireshark，不能只保存复现说明。

推荐流程：

```bash
sudo tcpdump -i lo0 -s 0 'tcp port 24680' -w /tmp/round2_loopback.pcap
./build/im-server/test_e2e
shasum -a 256 /tmp/round2_loopback.pcap
strings /tmp/round2_loopback.pcap | grep -E '<本次随机测试标记>'
```

每次测试生成随机且唯一的 ASCII marker，分别放入昵称、消息和测试 payload。不要只搜索固定中文，因为编码或工具显示方式可能影响结果。

### 8.2 应保存的证据

`kernel-round2-packet-capture-note.md` 增加：

- 执行时间和 commit；
- 抓包网卡、过滤条件和端口；
- pcap 大小与 SHA-256；
- TLS 1.3 application_data 统计；
- marker 搜索命令与真实输出；
- 明文 fail-close 测试的实际输出；
- pcap 存放位置（不提交含环境元数据的 pcap 到公开仓库）。

注意：服务端“成功解密 1040”的日志只能作为辅助证据，不能替代线路抓包。

---

## 9. R2-F07：验收报告修订

### 9.1 修复前的报告状态

在 F01～F06 完成前，`kernel-round2-acceptance.md` 顶部结论应改为：

> 第二轮 P3/P4 验收完成：并发加密、SPKI 接入、Android 真实端到端、三端独立 Golden 与真实抓包门禁均已关闭。

### 9.2 删除矛盾结论

当前正文同时存在“全部通过”“尚未整体完成”“补充后完成”三种结论。最终报告应直接合并正文，不再通过追加补充覆盖旧结论。

统一结构：

```text
1. 本轮范围
2. 实际代码交付
3. 测试矩阵与原始证据
4. 审查发现及修复
5. 遗留项
6. 唯一最终结论
```

### 9.3 设备身份表述

明确写入：

- 当前 C++ P-256 DeviceProof 可完成签名验证；
- 默认 device_id 和私钥是实例级临时值；
- 安装级稳定 device_id、Android Keystore 持久化、TokenLogin/Refresh 签名属于 P5；
- 当前实现不得描述为“完整可信设备体系”。

---

## 10. 最终复验清单

### 10.1 代码门禁

- [x] 加密、sequence、入队在同一串行 executor 上。
- [x] 8 线程 10,000 个加密帧无重号、跳号、丢失和重复。
- [x] ClientConfig/JNI 可配置 current/next SPKI pins。
- [x] 多级证书链只对叶子证书执行 pin 校验。
- [x] pin 为空、pin 错误、SAN 错误、过期证书全部 fail-close。
- [x] 安全失败后不发送业务帧、不向上抛未认证明文。
- [x] close/析构/迟到 handler 压力测试无 UAF、死锁和重复回调。

### 10.2 跨端门禁

- [x] C++ 生产实现独立消费 Golden。
- [x] Android/Kotlin 现状实现独立消费 Golden。
- [x] Server 生产 AppCrypto 独立消费 Golden。
- [x] 三端全部字段逐字节一致。
- [x] Android arm64 真实完成 TLS → App handshake → encrypted heartbeat → decrypt response。

### 10.3 证据门禁

- [x] Release tests 保持 `-UNDEBUG`，断言真实执行。
- [x] client_core 普通 + ASan/UBSan 全绿。
- [x] im_server 普通 + ASan/UBSan 全绿。
- [x] Android arm64 instrumentation 已通过；CheckJNI/HWASan 沿用第一轮通过证据且未退化。
- [x] pcap SHA-256 与 marker 搜索真实输出已记录。
- [x] 验收报告只有一个最终结论，无互相覆盖的补充结论。

---

## 11. 建议提交拆分

```text
fix(security): serialize encryption sequence and transport enqueue
fix(tls): wire SPKI pins and verify leaf certificate only
test(transport): add multithread encrypted 10k-frame stress coverage
test(security): make Android C++ and server consume shared golden vectors
test(android): add arm64 real TLS secure-channel heartbeat e2e
docs(security): record packet capture evidence and correct round2 verdict
```

每个提交都应能独立构建；不要把代码修复、测试补齐和报告改写压成一个无法审查的大提交。

## 12. 完成定义

第 10 节已经全部关闭，第二轮从“条件通过”更新为“完成”。设备身份持久化仍按既定范围进入 P5，并已在验收报告中保留限制说明。
