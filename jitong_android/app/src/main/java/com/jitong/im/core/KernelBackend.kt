package com.jitong.im.core

/**
 * 内核后端选择（灰度回退开关）。
 *
 * 规则（执行规划「全局约束」）：
 *   1. `KernelBackend` 在**进程启动前**确定，运行期间不可热切换；
 *   2. 一个进程只允许一个长连接核心处于活动状态；
 *   3. 回退必须重启进程（禁止运行中切换实现，否则会出现双写/双连）。
 *
 * 首阶段（P1/P2）Native 内核默认**关闭**：即使 .so 存在也不接管业务，
 * 保证当前 Kotlin 客户端行为不受影响；P13 灰度通过后再改为默认开启。
 */
enum class KernelBackend {
    /** 现有 Kotlin 实现（ImClient + Room + SecureChannel）。 */
    KOTLIN_LEGACY,

    /** C++ Native 内核。 */
    CPP_NATIVE,
}

object KernelBackendSelector {

    @Volatile
    private var initialized = false

    /**
     * 期望的后端。缺省 [KernelBackend.KOTLIN_LEGACY]。
     * 只有 `jitong.kernel.backend=native` 且 .so 加载成功时才会真正生效。
     */
    @Volatile
    var requested: KernelBackend = KernelBackend.KOTLIN_LEGACY
        private set

    /** 实际生效的后端：Native 只有在 .so 可用时才生效，否则自动降级。 */
    val effective: KernelBackend
        get() = if (requested == KernelBackend.CPP_NATIVE && NativeBindings.isLoaded) {
            KernelBackend.CPP_NATIVE
        } else {
            KernelBackend.KOTLIN_LEGACY
        }

    /** Native 内核是否可用（.so 加载成功）。 */
    val isNativeAvailable: Boolean get() = NativeBindings.isLoaded

    /**
     * 在 Application.onCreate 中调用一次，确定后端。
     * 之后调用会被忽略（进程内不可热切换）。
     */
    @Synchronized
    fun initialize(useNative: Boolean) {
        if (initialized) return
        requested = if (useNative) KernelBackend.CPP_NATIVE else KernelBackend.KOTLIN_LEGACY
        initialized = true
    }
}
