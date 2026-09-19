package com.jitong.im

import com.jitong.im.core.NativeServerConfig
import com.jitong.im.net.ImClient
import com.jitong.im.net.Protocol
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Native 分支通过 [NativeServerConfig] 获取服务端地址，禁止 import Legacy 网络包；
 * 本测试守护两处常量不漂移，避免 Native 与 Legacy 连到不同服务端。
 */
class NativeServerConfigTest {
    @Test
    fun `Native 服务端配置与 Legacy 常量一致`() {
        assertEquals(Protocol.TCP_PORT, NativeServerConfig.TCP_PORT)
        assertEquals(ImClient.DEFAULT_HOST, NativeServerConfig.DEFAULT_HOST)
    }
}
