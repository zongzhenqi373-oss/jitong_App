package com.jitong.im.net

import java.security.MessageDigest
import java.security.cert.X509Certificate
import java.util.Base64
import javax.net.ssl.SSLPeerUnverifiedException
import javax.net.ssl.SSLSession

/**
 * IM 两条 TLS 通道共用的 SPKI 固定配置。
 *
 * 固定的是 SubjectPublicKeyInfo 的 SHA-256，而不是易过期的整张证书。
 * 更换服务端密钥前，必须先随 App 更新把新公钥 pin 加入 [SHA256_PINS]，
 * 等客户端覆盖率满足要求后再切换服务端证书，最后移除旧 pin。
 */
object TlsPinning {
    const val CURRENT_PIN = "sha256/co0kqhe8Yl91qzqL9q9XOEUKNgSZABAgJEE78595QSE="

    private val SHA256_PINS = setOf(
        CURRENT_PIN.removePrefix("sha256/"),
    ).map(Base64.getDecoder()::decode)

    fun verify(session: SSLSession) {
        val certificates = try {
            session.peerCertificates
        } catch (error: SSLPeerUnverifiedException) {
            throw SSLPeerUnverifiedException("服务端未提供可验证证书").also { it.initCause(error) }
        }

        val matched = certificates
            .filterIsInstance<X509Certificate>()
            .map { MessageDigest.getInstance("SHA-256").digest(it.publicKey.encoded) }
            .any { actual -> SHA256_PINS.any { expected -> MessageDigest.isEqual(actual, expected) } }

        if (!matched) {
            throw SSLPeerUnverifiedException("服务端 SPKI 固定校验失败")
        }
    }
}
