package com.jitong.im.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.TextButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.AlertDialog
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.core.NativeKernelHost
import com.jitong.im.core.NativeMediaController
import com.jitong.im.core.NativeMessageController
import com.jitong.im.core.NativeServerConfig
import com.jitong.im.data.Prefs
import com.jitong.im.util.ImageCodec
import coil.compose.AsyncImage
import androidx.compose.ui.layout.ContentScale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** Native 模式的独立薄 UI 入口；不创建 MainViewModel、ImClient 或 Room writer。 */
object NativeAppRoot {
    @Composable
    operator fun invoke(ownerId: Int) {
        val context = LocalContext.current.applicationContext
        val scope = rememberCoroutineScope()
        val host = remember { NativeKernelHost(context, scope) }
        val state by host.state.collectAsStateWithLifecycle()
        val controllers by host.controllers.collectAsStateWithLifecycle()

        DisposableEffect(host) { onDispose { host.shutdown() } }
        LaunchedEffect(host) {
            if (host.setup(NativeServerConfig.DEFAULT_HOST, NativeServerConfig.DEFAULT_HOST,
                    NativeServerConfig.TCP_PORT)) {
                host.startWithSavedToken(Prefs.tel ?: ownerId.toString())
            }
        }

        when (state.phase) {
            NativeKernelHost.Phase.Ready -> controllers?.let { NativeHome(ownerId, it, host) }
                ?: Loading("正在创建 Native 业务控制器…")
            NativeKernelHost.Phase.AccountReady,
            NativeKernelHost.Phase.Stopped -> NativeLogin(host, state.error)
            NativeKernelHost.Phase.Authenticating -> Loading("正在认证…")
            NativeKernelHost.Phase.Activating -> Loading("正在解锁本地数据库…")
            NativeKernelHost.Phase.Kicked -> NativeLogin(host, state.error ?: "账号已在其他设备登录")
            NativeKernelHost.Phase.Failed -> Repair(state.error ?: "Native 内核启动失败")
        }
    }

    @Composable
    fun Repair(reason: String) {
        Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
            Text("本地数据需要修复", fontWeight = FontWeight.Bold)
            Text(reason, Modifier.padding(top = 12.dp))
            Text("为避免消息双写或回退丢失，已禁止自动启动 Legacy 后端。",
                Modifier.padding(top = 12.dp))
        }
    }

    @Composable
    private fun NativeLogin(host: NativeKernelHost, error: String?) {
        var account by remember { mutableStateOf(Prefs.tel.orEmpty()) }
        var password by remember { mutableStateOf("") }
        var nick by remember { mutableStateOf("") }
        var tab by remember { mutableStateOf(0) } // 0=登录 1=注册
        val context = LocalContext.current
        // 注册结果：1=成功（切回登录页并预填账号），2=昵称占用，3=手机号已注册，0=本地连接失败
        LaunchedEffect(host) {
            host.registerResults.collect { result ->
                when (result) {
                    1 -> {
                        Toast.makeText(context, "注册成功，请登录", Toast.LENGTH_SHORT).show()
                        tab = 0
                    }
                    2 -> Toast.makeText(context, "昵称已被占用", Toast.LENGTH_SHORT).show()
                    3 -> Toast.makeText(context, "该手机号已注册", Toast.LENGTH_SHORT).show()
                    else -> Toast.makeText(context, "注册失败：无法连接服务端", Toast.LENGTH_SHORT).show()
                }
            }
        }
        Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
            Text("Native 内核登录", fontWeight = FontWeight.Bold)
            Row(Modifier.padding(top = 8.dp)) {
                TextButton(onClick = { tab = 0 }) { Text(if (tab == 0) "【登录】" else "登录") }
                TextButton(onClick = { tab = 1 }) { Text(if (tab == 1) "【注册】" else "注册") }
            }
            error?.let { Text(it, Modifier.padding(top = 8.dp)) }
            if (tab == 1) {
                OutlinedTextField(nick, { nick = it }, label = { Text("昵称") },
                    modifier = Modifier.fillMaxWidth().padding(top = 12.dp))
            }
            OutlinedTextField(account, { account = it }, label = { Text("手机号") },
                modifier = Modifier.fillMaxWidth().padding(top = if (tab == 1) 8.dp else 12.dp))
            OutlinedTextField(password, { password = it }, label = { Text("密码") },
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp))
            if (tab == 0) {
                Button(onClick = { host.loginWithPassword(account.trim(), password) },
                    enabled = account.isNotBlank() && password.isNotBlank(),
                    modifier = Modifier.padding(top = 12.dp)) { Text("登录") }
            } else {
                Button(onClick = {
                    if (!host.register(nick.trim(), account.trim(), password)) {
                        Toast.makeText(context, "注册提交失败（账号忙或参数非法）",
                            Toast.LENGTH_SHORT).show()
                    }
                }, enabled = nick.isNotBlank() && account.isNotBlank() && password.isNotBlank(),
                    modifier = Modifier.padding(top = 12.dp)) { Text("注册") }
            }
        }
    }

    @Composable
    private fun NativeHome(ownerId: Int, controllers: NativeKernelHost.Controllers,
                           host: NativeKernelHost) {
        val conversations by controllers.messages.conversations.collectAsStateWithLifecycle()
        val friends by controllers.friends.friends.collectAsStateWithLifecycle()
        val requests by controllers.friends.requests.collectAsStateWithLifecycle()
        var selected by remember { mutableStateOf<Pair<Long, Long>?>(null) }
        var page by remember { mutableStateOf(HomePage.Conversations) }
        if (selected != null) {
            NativeChat(controllers.messages,controllers.media,selected!!.first,selected!!.second) {
                selected = null
            }
            return
        }
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Row {
                    TextButton(onClick = { page = HomePage.Conversations }) { Text("会话") }
                    TextButton(onClick = { page = HomePage.Friends }) { Text("好友") }
                    TextButton(onClick = { page = HomePage.Search }) { Text("搜索") }
                }
                Button(onClick = { host.logout() }) { Text("退出") }
            }
            when (page) {
                HomePage.Conversations -> LazyColumn {
                    items(conversations, key = { it.conversationId }) { conversation ->
                        Column(Modifier.fillMaxWidth().clickable {
                            selected = conversation.conversationId to conversation.peerId
                        }.padding(vertical = 12.dp)) {
                            Text("好友 ${conversation.peerId}")
                            Text(conversation.lastMessage)
                            if (conversation.unread > 0) Text("未读 ${conversation.unread}")
                        }
                    }
                }
                HomePage.Friends -> NativeFriends(ownerId, controllers, friends, requests) {
                    selected = conversationId(ownerId.toLong(), it) to it
                }
                HomePage.Search -> NativeSearch(controllers.messages) {
                    selected = it.conversationId to it.peerId
                }
            }
        }
    }

    @Composable
    private fun NativeFriends(ownerId: Int, controllers: NativeKernelHost.Controllers,
                              friends: List<com.jitong.im.core.JitongSdk.NativeFriend>,
                              requests: List<com.jitong.im.core.JitongSdk.NativeFriendRequest>,
                              openChat: (Long) -> Unit) {
        var nick by remember { mutableStateOf("") }
        Row(Modifier.fillMaxWidth()) {
            OutlinedTextField(nick, { nick = it }, label = { Text("好友昵称") },
                modifier = Modifier.weight(1f))
            Button(onClick = { controllers.friends.add(nick); nick = "" },
                enabled = nick.isNotBlank(), modifier = Modifier.padding(start = 8.dp)) {
                Text("添加")
            }
        }
        LazyColumn {
            items(requests, key = { it.requestId }) { request ->
                Column(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
                    Text("好友申请：${request.message.ifBlank { request.fromUserId.toString() }}")
                    if (request.direction == 0 && request.state == 0) Row {
                        Button(onClick = { controllers.friends.answer(request, true) }) { Text("同意") }
                        TextButton(onClick = { controllers.friends.answer(request, false) }) {
                            Text("拒绝")
                        }
                    }
                }
            }
            items(friends, key = { it.friendId }) { friend ->
                Row(Modifier.fillMaxWidth().clickable { openChat(friend.friendId) }
                    .padding(vertical = 12.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                    Column {
                        Text(friend.nick.ifBlank { friend.friendId.toString() })
                        Text(if (friend.online) "在线" else "离线")
                    }
                    TextButton(onClick = { controllers.friends.delete(friend.friendId) }) {
                        Text("删除", color = Color(0xFFD93025))
                    }
                }
            }
        }
        LaunchedEffect(ownerId) { controllers.friends.refresh() }
    }

    @Composable
    private fun NativeSearch(controller: NativeMessageController,
                             open: (com.jitong.im.core.JitongSdk.SearchHit) -> Unit) {
        var keyword by remember { mutableStateOf("") }
        var hits by remember { mutableStateOf(emptyList<com.jitong.im.core.JitongSdk.SearchHit>()) }
        LaunchedEffect(keyword) {
            hits = if (keyword.isBlank()) emptyList() else withContext(Dispatchers.IO) {
                controller.search(keyword.trim())
            }
        }
        OutlinedTextField(keyword, { keyword = it }, label = { Text("文本或拼音") },
            modifier = Modifier.fillMaxWidth())
        LazyColumn {
            items(hits, key = { it.msgId }) { hit ->
                Text(highlighted(hit), Modifier.fillMaxWidth().clickable { open(hit) }
                    .padding(vertical = 12.dp))
            }
        }
    }

    private fun highlighted(hit: com.jitong.im.core.JitongSdk.SearchHit) = buildAnnotatedString {
        var at = 0
        hit.highlights.sortedBy { it.startUtf16 }.forEach { range ->
            val start = range.startUtf16.coerceIn(at, hit.snippet.length)
            val end = range.endUtf16.coerceIn(start, hit.snippet.length)
            append(hit.snippet.substring(at, start))
            withStyle(SpanStyle(fontWeight = FontWeight.Bold, color = Color(0xFF1677FF))) {
                append(hit.snippet.substring(start, end))
            }
            at = end
        }
        append(hit.snippet.substring(at))
    }

    private fun conversationId(owner: Long, peer: Long): Long {
        val low=minOf(owner,peer);val high=maxOf(owner,peer)
        return (low shl 32) or (high and 0xffffffffL)
    }

    private enum class HomePage { Conversations, Friends, Search }

    @Composable
    private fun NativeChat(controller: NativeMessageController,media:NativeMediaController,
                           conversationId: Long, peerId: Long,back: () -> Unit) {
        val page by controller.messages(conversationId).collectAsStateWithLifecycle()
        val localMedia by media.local.collectAsStateWithLifecycle()
        var text by remember { mutableStateOf("") }
        var opened by remember { mutableStateOf<com.jitong.im.core.JitongSdk.NativeMessage?>(null) }
        val context=LocalContext.current
        val scope=rememberCoroutineScope()
        val pickImage=rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
            if(uri!=null)scope.launch {
                ImageCodec.loadAndCompress(context,uri)?.let { media.sendImage(conversationId,peerId,it) }
            }
        }
        val pickFile=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if(uri!=null)media.sendFile(conversationId,peerId,uri)
        }
        DisposableEffect(conversationId) {
            onDispose { controller.closeConversation(conversationId) }
        }
        LaunchedEffect(page.messages.size) {
            page.messages.maxOfOrNull { it.conversationSeq }?.takeIf { it > 0 }?.let {
                controller.markRead(conversationId,it)
            }
        }
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            Row(Modifier.fillMaxWidth()) {
                Button(onClick = back) { Text("返回") }
                TextButton(onClick = {
                    if (!controller.requestRoamHistory(conversationId, peerId)) {
                        Toast.makeText(context, "漫游拉取提交失败", Toast.LENGTH_SHORT).show()
                    }
                }, modifier = Modifier.padding(start = 8.dp)) { Text("漫游历史") }
            }
            LazyColumn(Modifier.weight(1f)) {
                items(page.messages, key = { it.msgId }) { message ->
                    when(message.type){
                        1 -> NativeImage(message,localMedia[message.msgId],media) { opened=message }
                        2 -> Text("📎 ${message.media.fileName}  ${message.media.fileSize} B",
                            Modifier.fillMaxWidth().clickable { media.ensureOriginal(message) }
                                .padding(vertical=10.dp))
                        else -> Text(message.content, Modifier.fillMaxWidth().padding(vertical = 6.dp))
                    }
                }
                if (page.hasMore) item {
                    Button(onClick = { controller.loadMore(conversationId) }) { Text("加载更多") }
                }
            }
            Row(Modifier.fillMaxWidth()) {
                OutlinedTextField(text, { text = it }, label = { Text("输入消息") },
                    modifier = Modifier.weight(1f))
                Button(onClick = {
                    val receipt = controller.sendText(conversationId, peerId, text.trim())
                    if (!receipt.accepted) {
                        Toast.makeText(context, "发送失败：${receipt.error ?: "未知错误"}",
                            Toast.LENGTH_SHORT).show()
                    } else scope.launch {
                        // 本地事务已提交；等待网络终态，失败/超时给用户反馈（消息本体不丢，可重试）。
                        val completion = controller.awaitOperation(receipt.operationId)
                        if (completion != null &&
                            completion.status != com.jitong.im.core.JitongSdk.OperationStatus.Ok) {
                            Toast.makeText(context,
                                "消息未送达：${completion.error ?: completion.status.name}（已保存，可重发）",
                                Toast.LENGTH_LONG).show()
                        }
                    }
                    text = ""
                }, enabled = text.isNotBlank(), modifier = Modifier.padding(start = 8.dp)) {
                    Text("发送")
                }
            }
            Row {
                TextButton(onClick={pickImage.launch(PickVisualMediaRequest(
                    ActivityResultContracts.PickVisualMedia.ImageOnly))}){Text("图片")}
                TextButton(onClick={pickFile.launch(arrayOf("*/*"))}){Text("文件")}
            }
            localMedia.filter { (key,value) ->
                key.startsWith("m-") &&
                    value.status==NativeMediaController.Status.Working
            }.forEach { (key,_) ->
                Row {
                    Text("媒体传输中",Modifier.weight(1f))
                    TextButton(onClick={media.cancel(key)}){Text("取消")}
                }
            }
        }
        opened?.let { message ->
            LaunchedEffect(message.msgId){
                val download=media.ensureOriginal(message)
                try { download.join() }
                finally { if(!download.isCompleted){media.cancel(message.msgId);download.cancel()} }
            }
            AlertDialog(onDismissRequest={media.cancel(message.msgId);opened=null},confirmButton={
                TextButton(onClick={media.cancel(message.msgId);opened=null}){Text("关闭")}
            },text={AsyncImage(model=localMedia[message.msgId]?.path?.takeIf{it.isNotBlank()}?.let(::File),
                contentDescription="原图",contentScale=ContentScale.Fit,
                modifier=Modifier.fillMaxWidth())})
        }
    }

    @Composable
    private fun NativeImage(message:com.jitong.im.core.JitongSdk.NativeMessage,
                            local:NativeMediaController.Local?,media:NativeMediaController,
                            open:()->Unit){
        LaunchedEffect(message.msgId){media.ensurePreview(message)}
        val ratio=if(message.media.width>0&&message.media.height>0)
            (message.media.width.toFloat()/message.media.height).coerceIn(.25f,4f) else 1f
        AsyncImage(model=local?.path?.takeIf{it.isNotBlank()}?.let(::File),contentDescription="图片",
            contentScale=ContentScale.Crop,modifier=Modifier.fillMaxWidth().aspectRatio(ratio).clickable(onClick=open))
    }

    @Composable
    private fun Loading(text: String) {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
            CircularProgressIndicator(Modifier.padding(24.dp))
            Text(text, Modifier.padding(horizontal = 24.dp))
        }
    }
}
