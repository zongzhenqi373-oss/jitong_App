package com.jitong.im

import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.jitong.im.ui.ChatScreen
import com.jitong.im.ui.ChatSearchScreen
import com.jitong.im.ui.FriendListScreen
import com.jitong.im.ui.LoginScreen
import com.jitong.im.ui.MainViewModel
import com.jitong.im.ui.Screen
import com.jitong.im.ui.theme.JitongTheme
import com.tencent.mmkv.MMKV

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MMKV.initialize(this) // KV 凭证存储（自动登录）
        setContent {
            JitongTheme {
                val vm: MainViewModel = viewModel()
                val appContext = applicationContext
                // 仅首次组合调用一次（attachContext/attachResolver 内部亦有幂等保护）。
                // 本地库现在要密码派生的密钥才能打开，改由 MainViewModel 在登录成功后自行构建 ChatStore。
                LaunchedEffect(Unit) {
                    vm.attachResolver(appContext.contentResolver)
                    vm.attachContext(appContext)
                }
                AppNav(vm)
            }
        }
    }
}

@Composable
private fun AppNav(vm: MainViewModel) {
    val screen by vm.screen.collectAsStateWithLifecycle()
    val context = LocalContext.current

    LaunchedEffect(Unit) {
        vm.toast.collect { Toast.makeText(context, it, Toast.LENGTH_LONG).show() }
    }

    when (screen) {
        Screen.Login -> LoginScreen(vm)
        Screen.FriendList -> FriendListScreen(vm)
        Screen.Chat -> {
            BackHandler { vm.backToFriends() }
            ChatScreen(vm)
        }
        Screen.ChatSearch -> {
            BackHandler { vm.backToChat() }
            ChatSearchScreen(vm)
        }
    }
}
