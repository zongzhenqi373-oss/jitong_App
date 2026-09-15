package com.jitong.im.core.platform

import android.content.Context
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult

/**
 * Native 数据库密钥平台桥（P6-T02 / S6 正式路径）。
 *
 * C++ 侧通过 `IPlatformKeyBridge` 只表达「我要一把 32 字节库密钥」这个请求；本类负责用
 * **当前登录态**的 passHash 从 [DbKeyManager]（PBKDF2(passHash) + AES-GCM 包装 realKey）
 * 解出 realKey。业务状态机仍在 C++，这里只提供 Android 独有的原子能力。
 *
 * 关键语义：
 *  - 无 passHash 时尝试已有设备包装；不可用则返回 null，不创建空密码包装；
 *  - 绝不拿 Access/Refresh Token 派生 key，也绝不静默回退到任何「永远可用」的密钥；
 *  - `deleteKey` 仅服务「清除本机数据」这类显式操作，登录/认证流程禁止自动调用。
 */
class NativeDbKeyPlatform(
    private val context: Context,
) {
    @Volatile
    private var passHash: String? = null

    /** 密码登录成功后由上层注入当前账号 passHash；登出/被踢/清理时置 `null`。 */
    fun setPassHash(hash: String?) {
        passHash = hash
    }

    /** C++ `IPlatformKeyBridge.loadKey` 对应实现；返回 `null` 表示密钥不可用。 */
    fun loadKey(ownerId: Int): ByteArray? {
        // 注意：**不能**在无 passHash 时直接 return null——那会让 ADR-01 的
        // deviceWrapper 分支永远不可达（Token 冷启动无法无感解锁）。
        // 传空串给 DbKeyManager，由其内部走「passHash → deviceWrapper → fail-close」三级路径。
        val hash = passHash ?: ""
        val result = if (hash.isEmpty()) DbKeyManager.unlockRealKey(context, ownerId)
                     else DbKeyManager.getOrCreateRealKey(context, ownerId, hash)
        return when (val r = result) {
            is DbKeyResult.Success -> r.key
            is DbKeyResult.Unavailable -> null
        }
    }

    /** C++ `IPlatformKeyBridge.deleteKey` 对应实现：显式「清除本机数据」。 */
    fun deleteKey(ownerId: Int) {
        DbKeyManager.clearLocalDatabase(context, ownerId)
    }
}
