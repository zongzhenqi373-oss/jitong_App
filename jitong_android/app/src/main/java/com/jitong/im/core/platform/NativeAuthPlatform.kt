package com.jitong.im.core.platform

import com.jitong.im.data.Prefs
import com.jitong.im.data.crypto.DeviceIdentity

/**
 * Native 认证内核使用的最小 Android 平台桥。
 *
 * 业务状态机仍在 C++；这里仅提供 Android 独有原子能力：稳定设备标识、
 * Keystore P-256 签名，以及 Keystore 加密的 opaque blob 持久化。
 * 所有方法都以失败返回空值/false，Native 侧据此 fail-close。
 */
class NativeAuthPlatform(
    private val secureKv: SecureKv = AndroidSecureKv(),
) {
    fun deviceId(): String = Prefs.deviceId

    fun publicKey(): ByteArray? = runCatching { DeviceIdentity.publicKey() }.getOrNull()

    fun sign(payload: ByteArray): ByteArray? =
        runCatching { DeviceIdentity.sign(payload) }.getOrNull()

    fun loadStore(): ByteArray? = secureKv.load(STORE_KEY)

    fun saveStore(blob: ByteArray): Boolean = secureKv.save(STORE_KEY, blob)

    companion object {
        private const val STORE_KEY = "native_account_tokens_v1"
    }
}
