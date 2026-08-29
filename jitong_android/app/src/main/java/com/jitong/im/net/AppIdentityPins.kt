package com.jitong.im.net

import java.util.Base64

/** 服务端应用层Ed25519身份公钥；私钥只存在于服务端。 */
object AppIdentityPins {
    private val keys = mapOf(
        1 to Base64.getDecoder().decode("hdThT8vmFpBUsYmo7jHa86Knht+cZFyulsdRYHNPUHI="),
    )

    fun publicKey(keyId: Int): ByteArray? = keys[keyId]?.copyOf()
}
