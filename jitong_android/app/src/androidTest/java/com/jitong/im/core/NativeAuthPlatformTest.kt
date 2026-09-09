package com.jitong.im.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.jitong.im.core.platform.AndroidSecureKv
import com.jitong.im.core.platform.NativeAuthPlatform
import com.tencent.mmkv.MMKV
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.BeforeClass
import org.junit.Test
import org.junit.runner.RunWith
import java.security.KeyFactory
import java.security.Signature
import java.security.spec.X509EncodedKeySpec

/** Android Keystore 与 Native 认证平台桥的真机/AVD 验收。 */
@RunWith(AndroidJUnit4::class)
class NativeAuthPlatformTest {

    @Test
    fun deviceIdentity_isStableAndSignatureVerifies() {
        val first = NativeAuthPlatform()
        val second = NativeAuthPlatform()
        assertTrue(first.deviceId().isNotBlank())
        assertEquals(first.deviceId(), second.deviceId())

        val payload = "jitong-native-auth-platform".toByteArray()
        val publicKey = requireNotNull(first.publicKey())
        val signature = requireNotNull(first.sign(payload))
        val key = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(publicKey))
        assertTrue(Signature.getInstance("SHA256withECDSA").run {
            initVerify(key)
            update(payload)
            verify(signature)
        })
        assertArrayEquals(publicKey, second.publicKey())
    }

    @Test
    fun encryptedStore_roundTripsAcrossBridgeInstances() {
        val kv = AndroidSecureKv()
        val key = "native_account_tokens_v1"
        val previous = kv.load(key)
        val marker = "token-store-${System.nanoTime()}".toByteArray()
        try {
            assertTrue(NativeAuthPlatform().saveStore(marker))
            assertArrayEquals(marker, NativeAuthPlatform().loadStore())
            assertNotEquals(marker.decodeToString(), MMKV.defaultMMKV().decodeString("jt_sec_$key"))
        } finally {
            if (previous == null) kv.erase(key) else assertTrue(kv.save(key, previous))
        }
    }

    @Test
    fun nativeAccountSetup_acceptsProductionPlatformBridgeWithoutConnecting() {
        val sdk = JitongSdk()
        assertTrue(sdk.start("localhost"))
        try {
            // setup 只组装 AccountSession；未表达登录意图前不会建立第二条连接。
            assertTrue(sdk.setupAccount("127.0.0.1", 6553))
            assertEquals(AccountState.LoggedOut, sdk.accountState.value)
        } finally {
            sdk.stop()
        }
    }

    companion object {
        @JvmStatic
        @BeforeClass
        fun initializeMmkv() {
            MMKV.initialize(InstrumentationRegistry.getInstrumentation().targetContext)
        }
    }
}
