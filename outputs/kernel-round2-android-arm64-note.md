# Android arm64 应用层安全通道验证说明（P4 §22.3.5）

> 目标：在 Android **arm64-v8a** 上验证 P4 应用层安全通道（四步握手 + 加密业务帧）
> 与 P-256 设备证明，并验证 App 进程到宿主机 im_server 的端到端网络链路。
>
> 执行日期：2026-09-08　设备：Pixel_7 AVD（Android 14，`ro.product.cpu.abi=arm64-v8a`）
> NDK：27.3.13750724

本轮分两部分，均已在 arm64 AVD 实跑通过。

## Part 1：进程内 arm64 真实握手（自动化，已通过）

新增 androidTest `com.jitong.im.core.SecureChannelHandshakeTest`，通过 JNI 调用 native
自检：

- `nativeSecureChannelHandshakeTest()` → `ClientSecureChannel::runLoopbackHandshakeSelfTest`：
  在同一进程用与服务端 `Session.cpp` **逐字节一致**的加密原语实现服务端半程，驱动真实
  `ClientSecureChannel` 走完整四步握手并做加密业务帧双向往返。
- `nativeDeviceProofSelfTest()`：P-256 生成 → 导出 X.509 SPKI DER → SHA256withECDSA 签名
  → 本地验签往返。

设备实测输出（`abi=arm64-v8a`，全步骤 `ok`）：

```text
deviceProof = {abi=arm64-v8a, generate=ok, public_key=ok, sign=ok, verify=ok, result=ok}
handshake   = {abi=arm64-v8a, ed25519_keygen=ok, client_hello=ok, parse_client_hello=ok,
               server_x25519=ok, server_random=ok, server_sign=ok, client_handle_server_hello=ok,
               server_x25519_derive=ok, server_transcript_hash=ok, server_salt=ok, server_hkdf=ok,
               parse_client_finished=ok, server_hmac_client=ok, verify_client_finished=ok,
               server_hmac_server=ok, client_handle_server_finished=ok, client_established=ok,
               client_encrypt=ok, parse_frame=ok, server_decrypt=ok, server_encrypt=ok,
               client_decrypt=ok, result=ok}
```

复现：

```bash
# 1) 启动 arm64 AVD（Apple Silicon 上 arm64-v8a 原生运行）
$ANDROID_HOME/emulator/emulator -avd Pixel_7 &

# 2) 跑 P4 握手 + 设备证明 androidTest
cd jitong_android
./gradlew :app:connectedDebugAndroidTest -x lint \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.SecureChannelHandshakeTest

# 3) 看设备内打印的分步报告
$ANDROID_HOME/platform-tools/adb -s emulator-5554 logcat -d -s JitongSecureChannel
```

> 同一个 `runLoopbackHandshakeSelfTest` 也在桌面 `test_secure_channel`（CTest）里跑，
> 确保该自检本身正确且与生产链路零漂移。

## Part 2：真实 socket 加密 Heartbeat（已通过）

新增 test-only JNI `nativeSocketHeartbeatTest()` 和 androidTest
`NativeSocketHeartbeatTest`。它不是进程内模拟，也不是裸 TCP 可达性测试，而是从 Android
App 进程经 `10.0.2.2:24680` 连接真实宿主机 `im_server`，直接复用生产 `ClientCore`：

```text
Android arm64 App
→ TCP socket
→ TLS 1.3 + CA/hostname/SPKI pin
→ AppClientHello/AppServerHello（Ed25519 验签）
→ X25519 + HKDF-SHA256 派生双向密钥
→ ClientFinished/ServerFinished
→ AppEncryptedFrame(1040) / AES-256-GCM HeartbeatRq
→ 服务端解密并返回加密 HeartbeatRs
→ Android 解密；超过 7 个心跳周期仍保持连接
```

arm64 AVD 实测结果：

```text
{abi=arm64-v8a, config=ok, tls_and_app_handshake=ok,
 marker_probe_enqueued=ok, encrypted_heartbeat_roundtrip=ok, result=ok}
```

服务端同步确认：

```text
handshake success: TLSv1.3 cipher: TLS_AES_256_GCM_SHA384
[APP-SEC] 应用层安全握手成功 sessionBytes=16
[APP-SEC] 已认证解密业务帧 seq=1 innerType=1010 payloadBytes=32
[APP-SEC] 已认证解密业务帧 seq=2..8 innerType=1010 payloadBytes=0
```

第一帧的 32 字节 payload 是 F06 唯一测试 marker；pcap 中无法搜索到该 marker，证明它
并未以明文出现在真实线路上。

复现：

```bash
# 宿主机启动 im_server（绑定 0.0.0.0，AVD 经 10.0.2.2 可达）
CFG=build/im-server/config
./build/im-server/im_server 24680 /tmp/im_android_e2e.db \
  "$CFG/test_server.crt" "$CFG/test_server.key" 24681 "$CFG/test_app_identity_private.pem" &

cd jitong_android
./gradlew :app:connectedDebugAndroidTest -x lint \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.HostServerReachabilityTest
```

### 与 P5 登录迁移的边界

本入口仅用于 P4 验收，不向 UI 暴露登录业务。正式 `nativeConnect/nativeLogin/token refresh`
仍由 P5 迁移；但 P4 要求的真实 socket、TLS、应用握手、加密业务请求和解密响应已经闭环。

## 证据

```text
outputs/kernel-round2-android-arm64.log   arm64 握手/设备证明报告 + 可达性 + 宿主机连接日志
outputs/kernel-round2-android-socket-e2e-final.log  真实 socket Heartbeat androidTest 日志
outputs/kernel-round2-android-socket-server.log     同次运行的服务端解密日志
outputs/kernel-round2-android-arm64.pcap  AVD 侧真实线路抓包
jitong_android/app/src/androidTest/java/com/jitong/im/core/SecureChannelHandshakeTest.kt
jitong_android/app/src/androidTest/java/com/jitong/im/core/NativeSocketHeartbeatTest.kt
scripts/run_round2_android_e2e.sh
```
