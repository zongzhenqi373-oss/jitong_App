package com.jitong.im.core.platform

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * [NetworkObserver] 的 Android 实现。
 *
 * 只上报「网络可用 / 丢失」，**不做重连决策**：
 * 何时重连、退避多久、用哪种凭证重登，全部由 C++ ConnectionManager 决定（P3/P5）。
 */
class AndroidNetworkObserver(
    context: Context,
    private val scope: CoroutineScope,
) : NetworkObserver {

    private val manager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager

    @Volatile
    private var callback: ((NetworkObserver.State) -> Unit)? = null
    private val registered = AtomicBoolean(false)

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            emit(NetworkObserver.State.Available)
        }

        override fun onLost(network: Network) {
            emit(NetworkObserver.State.Lost)
        }
    }

    override fun current(): NetworkObserver.State {
        val cm = manager ?: return NetworkObserver.State.Unknown
        val active = cm.activeNetwork ?: return NetworkObserver.State.Lost
        val caps = cm.getNetworkCapabilities(active) ?: return NetworkObserver.State.Unknown
        val hasInternet = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
            caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
        return if (hasInternet) NetworkObserver.State.Available else NetworkObserver.State.Lost
    }

    override fun setCallback(callback: (NetworkObserver.State) -> Unit) {
        this.callback = callback
        val cm = manager ?: return
        if (!registered.compareAndSet(false, true)) return
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        if (runCatching { cm.registerNetworkCallback(request, networkCallback) }.isFailure) {
            registered.set(false)
        }
    }

    override fun close() {
        callback = null
        if (registered.compareAndSet(true, false)) {
            runCatching { manager?.unregisterNetworkCallback(networkCallback) }
        }
    }

    /** 回调可能来自 Binder 线程，统一切到后台线程再上抛，避免在 Binder 线程做重活。 */
    private fun emit(state: NetworkObserver.State) {
        scope.launch(Dispatchers.IO) { callback?.invoke(state) }
    }
}
