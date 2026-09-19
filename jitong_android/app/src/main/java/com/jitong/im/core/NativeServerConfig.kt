package com.jitong.im.core

/**
 * Native 分支的服务端接入配置。
 *
 * 独立于 `net.ImClient`/`net.Protocol`：Native 薄 UI 不得 import 任何 Legacy
 * 网络/存储符号（含常量），由 `verifyNativeUiDependencies` 静态门禁保证。
 * 值必须与 `net.Protocol.TCP_PORT` / `net.ImClient.DEFAULT_HOST` 保持一致；
 * 该一致性由 [com.jitong.im.core.NativeServerConfigTest] 单测守护。
 */
object NativeServerConfig {
    const val DEFAULT_HOST = "10.0.2.2"
    const val TCP_PORT = 24563
}
