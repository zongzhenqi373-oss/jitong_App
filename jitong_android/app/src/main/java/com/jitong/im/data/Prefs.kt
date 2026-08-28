package com.jitong.im.data

import com.jitong.im.data.crypto.PasswordVault
import com.jitong.im.data.crypto.TokenVault
import com.jitong.im.data.crypto.TokenVault.TokenSession
import com.tencent.mmkv.MMKV

/**
 * KV 凭证存储（MMKV，对齐 QQNT 双存储：MMKV 管 KV/凭证，Room 管消息）。
 * “记住账号密码”：勾选后持久化手机号 + 密码（密码经 PasswordVault/Keystore 加密后落盘，
 * 仅用于登录页回填），只要勾选着，退出登录/被踢/重开 App 都不清空；不勾选则不保存。
 * 注意：不做自动登录，回填后仍需用户手动点登录。
 */
object Prefs {
    private val kv: MMKV by lazy { MMKV.defaultMMKV() }

    var tel: String?
        get() = kv.decodeString("tel", null)?.takeIf { it.isNotEmpty() }
        set(v) {
            if (v == null) kv.removeValueForKey("tel") else kv.encode("tel", v)
        }

    /** 记住的密码：落盘是 Keystore 加密后的字节，取出来时在内存里解密成明文用于登录页回填 */
    var pass: String?
        get() = kv.decodeBytes("passEnc")?.let { PasswordVault.decrypt(it) }
        set(v) {
            if (v == null) kv.removeValueForKey("passEnc") else kv.encode("passEnc", PasswordVault.encrypt(v))
        }

    /** 是否记住账号密码（登录页勾选框持久化） */
    var remember: Boolean
        get() = kv.decodeBool("remember", false)
        set(v) = kv.encode("remember", v).let {}

    /**
     * 清除保存的账号密码。仅在“未勾选记住”时才真正清除；
     * 勾选了记住则保留（退出登录/被踢/重开都不清空）。
     */
    fun clearCredentialsIfNotRemember() {
        if (!remember) {
            kv.removeValuesForKeys(arrayOf("tel", "passEnc"))
        }
    }

    /**
     * 生成并保存 deviceId
     * deviceId 不要使用手机号，也不建议直接使用 Android 硬件标识。首次启动生成随机 UUID，之后一直复用
     */
    val deviceId: String
        get() {
            val saved = kv.decodeString("deviceId")
            if (!saved.isNullOrEmpty()) return saved

            val created = java.util.UUID.randomUUID().toString()
            kv.encode("deviceId", created)
            return created
        }

    /*保存Token的方法*/
    fun saveTokenSession(session: TokenSession) {
        val plain = listOf(
            session.userId.toString(),
            session.sessionId,
            session.accessToken,
            session.refreshToken,
            session.accessExpiresAt.toString(),
            session.refreshExpiresAt.toString(),
        ).joinToString("\n")

        kv.encode(
            "tokenSessionEnc",
            TokenVault.encrypt(plain),
        )
    }

    /*Token读取方法*/
    fun loadTokenSession(): TokenSession? {
        val encrypted = kv.decodeBytes("tokenSessionEnc") ?: return null
        val plain = TokenVault.decrypt(encrypted) ?: run {
            clearTokenSession()
            return null
        }

        val values = plain.split('\n')
        if (values.size != 6) {
            clearTokenSession()
            return null
        }

        return runCatching {
            TokenSession(
                userId = values[0].toInt(),
                sessionId = values[1],
                accessToken = values[2],
                refreshToken = values[3],
                accessExpiresAt = values[4].toLong(),
                refreshExpiresAt = values[5].toLong(),
            )
        }.getOrElse {
            clearTokenSession()
            null
        }
    }

    /*Token清除方法*/
    fun clearTokenSession() {
        kv.removeValuesForKeys(arrayOf("tokenSessionEnc", "pendingRefreshRequestId"))
    }

    var pendingRefreshRequestId: String?
        get() = kv.decodeString("pendingRefreshRequestId", null)?.takeIf { it.isNotBlank() }
        set(value) {
            if (value == null) kv.removeValueForKey("pendingRefreshRequestId")
            else kv.encode("pendingRefreshRequestId", value)
        }
}
