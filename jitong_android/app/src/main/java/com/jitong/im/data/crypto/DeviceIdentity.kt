package com.jitong.im.data.crypto

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.Signature
import java.security.spec.ECGenParameterSpec

/** Android Keystore 中不可导出的设备 P-256 私钥。 */
object DeviceIdentity {
    private const val PROVIDER = "AndroidKeyStore"
    private const val ALIAS = "jitong_device_identity_p256_v1"

    fun publicKey(): ByteArray = keyPair().certificate.publicKey.encoded

    fun sign(message: ByteArray): ByteArray = Signature.getInstance("SHA256withECDSA").run {
        initSign(keyPair().privateKey)
        update(message)
        sign()
    }

    private fun keyPair(): KeyStore.PrivateKeyEntry {
        val store = KeyStore.getInstance(PROVIDER).apply { load(null) }
        (store.getEntry(ALIAS, null) as? KeyStore.PrivateKeyEntry)?.let { return it }
        val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
            .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
            .setDigests(KeyProperties.DIGEST_SHA256)
            .setUserAuthenticationRequired(false)
            .build()
        KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, PROVIDER).apply {
            initialize(spec)
            generateKeyPair()
        }
        return store.getEntry(ALIAS, null) as KeyStore.PrivateKeyEntry
    }
}
