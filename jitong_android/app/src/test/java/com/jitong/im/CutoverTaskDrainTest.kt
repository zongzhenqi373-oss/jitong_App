package com.jitong.im

import com.jitong.im.data.CutoverTaskDrain
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CutoverTaskDrainTest {
    @Test fun waitsForExistingWriteAndDerivedWrite() = runBlocking {
        val parent = SupervisorJob()
        val scope = CoroutineScope(parent + Dispatchers.Default)
        var persisted = false
        val observed = CompletableDeferred<Boolean>()
        scope.launch {
            delay(20)
            scope.launch { delay(20); persisted = true }
        }
        val result = scope.launch {
            observed.complete(CutoverTaskDrain.awaitIdle(parent, currentCoroutineContext()[Job], 1_000) { false })
        }
        result.join()
        assertTrue(observed.await())
        assertTrue(persisted)
        scope.cancel()
    }

    @Test fun rejectsOutstandingMediaAndTimeout() = runBlocking {
        val parent = SupervisorJob()
        val scope = CoroutineScope(parent + Dispatchers.Default)
        val media = scope.launch { delay(10_000) }
        val observed = CompletableDeferred<Boolean>()
        val result = scope.launch {
            observed.complete(CutoverTaskDrain.awaitIdle(parent, currentCoroutineContext()[Job], 30) { true })
        }
        result.join()
        assertFalse(observed.await())
        media.cancel()
        scope.cancel()
    }
}
