package com.jitong.im.core.platform

import com.jitong.im.data.crypto.DeviceIdentity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

/**
 * [KeystoreProvider] 的 Android 实现：复用已有的 Keystore P-256 设备密钥。
 *
 * 只做两件事：取公钥、签名。签名原文（canonical bytes）由 C++ DeviceProofService
 * 构造，这里不理解其业务含义。
 */
class AndroidKeystoreProvider(
    private val scope: CoroutineScope,
    private val defaultAlias: String = "jitong_device_identity_p256_v1",
) : KeystoreProvider {

    override fun publicKey(alias: String): ByteArray? =
        runCatching { DeviceIdentity.publicKey() }.getOrNull()

    override fun signAsync(
        alias: String,
        payload: ByteArray,
        callback: (ok: Boolean, signature: ByteArray?) -> Unit,
    ) {
        // Keystore 签名涉及 IPC 与可能的 StrongBox，必须在后台线程执行
        scope.launch(Dispatchers.IO) {
            val result = runCatching { DeviceIdentity.sign(payload) }
            callback(result.isSuccess, result.getOrNull())
        }
    }
}
