package com.jitong.im.data

import android.content.Context
import com.jitong.im.core.NativeBindings
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import java.io.File

/** 只记录用户明确预约的单向切换；没有预约时冷启动绝不执行迁移。 */
object ColdStartCutoverRequest {
    data class Pending(val ownerId:Int,val epoch:Long)
    private const val PREF = "jitong_cold_start_cutover"

    fun pending(context:Context):Pending? {
        val p=context.getSharedPreferences(PREF,Context.MODE_PRIVATE)
        val owner=p.getInt("owner",0)
        val epoch=p.getLong("epoch",0)
        return if(owner>0&&epoch>0)Pending(owner,epoch) else null
    }

    /** 预约前只做只读检查；不建 Native 库，不写 journal，不触发停写。 */
    fun schedule(context:Context,ownerId:Int):String? {
        if(ownerId<=0)return "账号无效"
        if(pending(context)!=null)return "已有切换预约"
        if(Prefs.loadTokenSession()?.userId!=ownerId)return "登录账号与本地凭据不一致"
        if(!NativeBindings.isLoaded)return "Native 内核不可用"
        val room=File(context.getDatabasePath("jitong_$ownerId.db").absolutePath)
        if(!room.isFile)return "旧消息库不存在"
        val root=File(context.filesDir,"native_kernel")
        val native=File(File(root,"native_db"),"account_$ownerId.db")
        if(native.exists())return "已存在 Native 数据库，需先检查切换状态"
        val key=DbKeyManager.unlockRealKey(context,ownerId)
        if(key !is DbKeyResult.Success)return "旧消息库密钥无法在冷启动解锁"
        key.key.fill(0)
        val epoch=System.currentTimeMillis().coerceAtLeast(1)
        val saved=context.getSharedPreferences(PREF,Context.MODE_PRIVATE).edit()
            .putInt("owner",ownerId).putLong("epoch",epoch).commit()
        return if(saved)null else "切换预约未能持久化"
    }

    fun clear(context:Context) {
        context.getSharedPreferences(PREF,Context.MODE_PRIVATE).edit().clear().commit()
    }
}
