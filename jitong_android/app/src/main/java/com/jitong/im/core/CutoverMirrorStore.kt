package com.jitong.im.core

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey

/** Native cutover journal 的最小认证镜像。MAC 不通过时只能 Repair，禁止猜测或回退 Room。 */
class CutoverMirrorStore(context: Context) {
    data class Evidence(
        val ownerId: Long,
        val epoch: Long,
        val state: Int,
        val highWater: Long,
        val schemaVersion: Int,
        val keyId: String,
        val summary: String,
        val updatedAt: Long,
    )

    sealed interface ReadResult {
        data object Missing : ReadResult
        data class Valid(val evidence: Evidence) : ReadResult
        data class Corrupt(val reason: String) : ReadResult
    }

    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    @Synchronized
    fun write(evidence: Evidence): Boolean {
        if (!valid(evidence)) return false
        val mac = runCatching { sign(encode(evidence)) }.getOrNull() ?: return false
        return prefs.edit()
            .putLong("owner", evidence.ownerId)
            .putLong("epoch", evidence.epoch)
            .putInt("state", evidence.state)
            .putLong("high_water", evidence.highWater)
            .putInt("schema", evidence.schemaVersion)
            .putString("key_id", evidence.keyId)
            .putString("summary", evidence.summary)
            .putLong("updated_at", evidence.updatedAt)
            .putString("mac", Base64.encodeToString(mac, Base64.NO_WRAP))
            .commit()
    }

    @Synchronized
    fun read(): ReadResult {
        if (!prefs.contains("mac")) return ReadResult.Missing
        val evidence = Evidence(
            prefs.getLong("owner", 0), prefs.getLong("epoch", 0), prefs.getInt("state", -1),
            prefs.getLong("high_water", -1), prefs.getInt("schema", 0),
            prefs.getString("key_id", "").orEmpty(), prefs.getString("summary", "").orEmpty(),
            prefs.getLong("updated_at", 0),
        )
        if (!valid(evidence)) return ReadResult.Corrupt("镜像字段非法")
        val stored = runCatching {
            Base64.decode(prefs.getString("mac", ""), Base64.NO_WRAP)
        }.getOrNull() ?: return ReadResult.Corrupt("MAC 编码非法")
        val actual = runCatching { sign(encode(evidence)) }.getOrNull()
            ?: return ReadResult.Corrupt("Keystore HMAC 不可用")
        return if (MessageDigest.isEqual(stored, actual)) ReadResult.Valid(evidence)
        else ReadResult.Corrupt("MAC 校验失败")
    }

    /** 只供显式清除本机数据/测试；正常回退和登出不得删除镜像。 */
    @Synchronized
    fun clearExplicitly(): Boolean = prefs.edit().clear().commit()

    private fun valid(e: Evidence) = e.ownerId > 0 && e.epoch > 0 && e.state in 0..3 &&
        e.highWater >= 0 && e.schemaVersion > 0 && e.keyId.isNotBlank() && e.updatedAt > 0

    private fun encode(e: Evidence): ByteArray {
        val key = e.keyId.toByteArray(Charsets.UTF_8)
        val summary = e.summary.toByteArray(Charsets.UTF_8)
        return ByteBuffer.allocate(8 + 8 + 4 + 8 + 4 + 4 + key.size + 4 + summary.size + 8)
            .order(ByteOrder.BIG_ENDIAN)
            .putLong(e.ownerId).putLong(e.epoch).putInt(e.state).putLong(e.highWater)
            .putInt(e.schemaVersion).putInt(key.size).put(key).putInt(summary.size).put(summary)
            .putLong(e.updatedAt).array()
    }

    private fun sign(bytes: ByteArray): ByteArray = Mac.getInstance(ALGORITHM).run {
        init(loadOrCreateKey())
        doFinal(bytes)
    }

    private fun loadOrCreateKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(ALGORITHM, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                ALIAS, KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
            ).setDigests(KeyProperties.DIGEST_SHA256).build(),
        )
        return generator.generateKey()
    }

    internal companion object {
        const val PREFS = "jitong_cutover_mirror_v1"
        private const val ALIAS = "jitong.cutover.hmac.v1"
        private const val ALGORITHM = "HmacSHA256"
    }
}
