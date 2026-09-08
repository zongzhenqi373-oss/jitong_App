package com.jitong.im.core.platform

import android.util.Base64
import com.jitong.im.data.crypto.TokenVault
import com.tencent.mmkv.MMKV

/**
 * [SecureKv] 的 Android 实现：MMKV 存密文，密钥由 Android Keystore 保护。
 *
 * 平台侧**不理解** blob 的业务含义（内核会自己序列化 TokenSession 等结构），
 * 只负责「存进去是什么，取出来还是什么」。
 */
class AndroidSecureKv(
    private val kv: MMKV = MMKV.defaultMMKV() ?: MMKV.mmkvWithID("jitong_secure_kv"),
    private val keyPrefix: String = "jt_sec_",
) : SecureKv {

    override fun load(key: String): ByteArray? {
        val encoded = kv.decodeString(keyPrefix + key) ?: return null
        return runCatching {
            val encrypted = Base64.decode(encoded, Base64.DEFAULT)
            val plainText = TokenVault.decrypt(encrypted) ?: return null
            Base64.decode(plainText, Base64.DEFAULT)
        }.getOrNull()
    }

    override fun save(key: String, blob: ByteArray): Boolean =
        runCatching {
            val plainText = Base64.encodeToString(blob, Base64.DEFAULT)
            val encrypted = TokenVault.encrypt(plainText)
            kv.encode(keyPrefix + key, Base64.encodeToString(encrypted, Base64.DEFAULT))
        }.isSuccess

    override fun erase(key: String) {
        kv.removeValueForKey(keyPrefix + key)
    }
}
