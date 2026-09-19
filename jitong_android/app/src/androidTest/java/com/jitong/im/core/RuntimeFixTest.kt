package com.jitong.im.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class RuntimeFixTest {
    @Test fun mediaCancellationHandlesHaveIndependentLifecycle() {
        assertTrue(NativeBindings.isLoaded)
        val handle=NativeBindings.nativeCreate("im.example.com")
        assertTrue(handle!=0L)
        val first=NativeBindings.nativeBeginMediaOperation(handle)
        val second=NativeBindings.nativeBeginMediaOperation(handle)
        assertTrue(first>0L&&second>first)
        NativeBindings.nativeCancelMediaOperation(handle,first)
        NativeBindings.nativeEndMediaOperation(handle,first)
        NativeBindings.nativeEndMediaOperation(handle,second)
        NativeBindings.nativeDestroy(handle)
        assertEquals(0L,NativeBindings.nativeBeginMediaOperation(handle))
        NativeBindings.nativeCancelMediaOperation(handle,first)
        NativeBindings.nativeEndMediaOperation(handle,second)
    }

    @Test fun duplicateCreatePreservesRuntimeAndRejectsAccountReplacement() {
        assertTrue("Native must load for this regression", NativeBindings.isLoaded)
        val handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue(handle != 0L)
        try {
            assertTrue(NativeBindings.nativeCreateRuntime(handle, 999003))
            val runtimeSink = NativeRuntimeSink(
                invalidated = { _, _, _, _ -> },
                completed = { _, _, _, _, _ -> },
            )
            assertTrue("Runtime event JNI signatures must bind",
                NativeBindings.nativeSetRuntimeEventSink(handle, runtimeSink))
            val generation = NativeBindings.nativeStartRuntime(handle)
            assertTrue(generation > 0)
            assertTrue(NativeBindings.nativeCreateRuntime(handle, 999003))
            assertEquals(generation, NativeBindings.nativeStartRuntime(handle))
            assertFalse(NativeBindings.nativeCreateRuntime(handle, 999004))
            NativeBindings.nativeStopRuntime(handle)
            assertEquals("Stopped", NativeBindings.nativeGetRuntimeState(handle))
            assertTrue(NativeBindings.nativeStartRuntime(handle) > generation)
        } finally { NativeBindings.nativeDestroy(handle) }
        assertFalse(NativeBindings.nativeCreateRuntime(handle, 999003))
    }

    @Test fun destroyRuntimeAllowsAccountReplacementWithoutSecondCore() {
        assertTrue("Native must load for this regression", NativeBindings.isLoaded)
        val handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue(handle != 0L)
        try {
            assertTrue(NativeBindings.nativeCreateRuntime(handle, 999003))
            assertTrue(NativeBindings.nativeStartRuntime(handle) > 0)
            NativeBindings.nativeDestroyRuntime(handle)
            assertEquals("None", NativeBindings.nativeGetRuntimeState(handle))
            assertTrue(NativeBindings.nativeCreateRuntime(handle, 999004))
            assertTrue(NativeBindings.nativeStartRuntime(handle) > 0)
        } finally {
            NativeBindings.nativeDestroy(handle)
        }
    }
}
