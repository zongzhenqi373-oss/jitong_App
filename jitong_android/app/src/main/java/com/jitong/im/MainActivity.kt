package com.jitong.im

import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.material3.Button
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
import com.jitong.im.ui.NativeAppRoot

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            JitongTheme {
                when (val startup = (application as JitongApplication).startup) {
                    JitongApplication.Startup.Legacy -> {
                        // 只有 Legacy 分支才创建旧 ViewModel，避免 Native 分支暗中启动 ImClient/Room。
                        val vm: MainViewModel = viewModel()
                        val appContext = applicationContext
                        LaunchedEffect(Unit) {
                            vm.attachResolver(appContext.contentResolver)
                            vm.attachContext(appContext)
                        }
                        AppNav(vm)
                    }
                    is JitongApplication.Startup.Native -> NativeAppRoot(startup.ownerId)
                    is JitongApplication.Startup.Migrating -> {
                        LaunchedEffect(startup.ownerId) {
                            (application as JitongApplication).completePendingCutover()
                        }
                        Column {
                            CircularProgressIndicator()
                            Text("正在迁移本地消息，请勿关闭应用")
                        }
                    }
                    is JitongApplication.Startup.Repair -> NativeAppRoot.Repair(startup.reason)
                }
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
        Screen.Cutover -> {
            val status by vm.cutoverStatus.collectAsStateWithLifecycle()
            BackHandler { }
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally) {
                Text(status)
                if (status.startsWith("旧任务已排空")) Text("请重新打开应用，迁移将在冷启动时进行")
            }
        }
    }
}
