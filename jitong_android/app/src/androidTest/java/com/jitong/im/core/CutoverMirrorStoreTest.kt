package com.jitong.im.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CutoverMirrorStoreTest {
    @Test fun hmacBindsEveryRecoveryFieldAndResolverFailsClosed() {
        val context=ApplicationProvider.getApplicationContext<Context>()
        val store=CutoverMirrorStore(context)
        store.clearExplicitly()
        val evidence=CutoverMirrorStore.Evidence(9001,7,2,123,10,"key-1","sum",1000)
        assertTrue(store.write(evidence))
        assertEquals(evidence,(store.read() as CutoverMirrorStore.ReadResult.Valid).evidence)

        // 模拟磁盘/调试工具篡改任意恢复字段，MAC 必须失败。
        context.getSharedPreferences(CutoverMirrorStore.PREFS,Context.MODE_PRIVATE)
            .edit().putLong("high_water",124).commit()
        val corrupt=store.read()
        assertTrue(corrupt is CutoverMirrorStore.ReadResult.Corrupt)
        val action=CutoverRecoverySelector.resolve(
            9001,
            CutoverRecoverySelector.DbEvidence(true,7,2,123,10,"key-1","sum",1000),
            corrupt,nativeExists=true,nativeLoadable=true,
        )
        assertEquals(CutoverRecoverySelector.Action.Repair,action)
        store.clearExplicitly()
    }
}
