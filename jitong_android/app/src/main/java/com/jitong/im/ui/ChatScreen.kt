package com.jitong.im.ui

import android.graphics.BitmapFactory
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import coil.compose.AsyncImage
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.util.ImageCodec
import com.jitong.im.ui.theme.JitongBlue
import com.jitong.im.ui.theme.PageBackground
import com.jitong.im.ui.theme.PaleBlue
import com.jitong.im.ui.theme.SecondaryText
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(vm: MainViewModel) {
    val peer by vm.chatPeer.collectAsStateWithLifecycle()
    val allMessages by vm.messages.collectAsStateWithLifecycle()
    val jumpTarget by vm.chatJumpTarget.collectAsStateWithLifecycle()
    val aiReplyState by vm.aiReplyState.collectAsStateWithLifecycle()
    val p = peer ?: return
    val conv = allMessages[p.id].orEmpty()
    // LazyColumn 使用反向数据 + reverseLayout，让最新消息天然锚定输入框上方。
    // 键盘改变可视高度时从列表顶部收缩，不会把末条强制对齐到消息区顶部。
    val displayMessages = conv.asReversed()

    val listState = rememberLazyListState()
    val density = LocalDensity.current
    val imeBottom = WindowInsets.ime.getBottom(density)
    // 仅当"最新一条"变化（新消息到底部）时才滚到底；上拉加载更早消息不改变末条 id，不触发滚动，
    // 避免把用户从顶部拽回底部（对齐漫游设计决策 7：简单版不做滚动锚点，允许轻微跳动）
    LaunchedEffect(conv.lastOrNull()?.msgId) {
        if (conv.isNotEmpty()) listState.animateScrollToItem(0)
    }
    // 键盘弹出只会压缩 LazyColumn 的可视高度，不会改变消息数据；显式滚到底部，
    // 确保最后一条消息连同输入区一起位于 IME 上方，而不是留在被裁剪的底部区域。
    LaunchedEffect(imeBottom, p.id) {
        if (imeBottom > 0 && conv.isNotEmpty()) {
            listState.scrollToItem(0)
        }
    }
    // reverseLayout 下，最早消息位于数据尾部；滚到物理顶部时最后一个反向索引可见。
    LaunchedEffect(listState, p.id, displayMessages.size) {
        snapshotFlow {
            listState.layoutInfo.visibleItemsInfo.maxOfOrNull { it.index } ?: 0
        }
            .distinctUntilChanged()
            .collect { idx ->
                if (displayMessages.isNotEmpty() && idx >= displayMessages.lastIndex) {
                    vm.loadMoreHistory(p.id)
                }
            }
    }

    var input by rememberSaveable { mutableStateOf("") }
    var showPanel by remember { mutableStateOf(false) }
    var showTopMenu by remember { mutableStateOf(false) }
    var showDeleteConfirm by remember { mutableStateOf(false) }
    val keyboard = LocalSoftwareKeyboardController.current
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    if (showDeleteConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            title = { Text("删除好友") },
            text = { Text("确定删除 ${p.nick} 吗？聊天记录会保留。") },
            confirmButton = {
                TextButton(onClick = {
                    showDeleteConfirm = false
                    vm.deleteCurrentFriend()
                }) { Text("删除", color = Color(0xFFD93025)) }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) { Text("取消") }
            },
        )
    }

    LaunchedEffect(jumpTarget, conv.size) {
        val msgId = jumpTarget ?: return@LaunchedEffect
        val originalIndex = conv.indexOfFirst { it.msgId == msgId }
        if (originalIndex >= 0) {
            listState.animateScrollToItem(conv.lastIndex - originalIndex)
        } else {
            vm.notify("该消息尚未加载到当前聊天")
        }
        vm.consumeChatJumpTarget()
    }

    // 系统相册选择器（Photo Picker，无需存储权限）
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) {
            scope.launch {
                val compressed = ImageCodec.loadAndCompress(context, uri)
                if (compressed != null) {
                    vm.sendImage(compressed.bytes, compressed.w, compressed.h)
                } else {
                    vm.notify("图片读取失败")
                }
            }
        }
    }

    // SAF 选文件（任意 MIME），交给 vm.sendFile 走上传状态机
    val pickFile = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) vm.sendFile(uri, context.contentResolver)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                colors = androidx.compose.material3.TopAppBarDefaults.topAppBarColors(containerColor = Color.White),
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Avatar(id = p.id, nick = p.nick, size = 36.dp)
                        Spacer(Modifier.width(10.dp))
                        Column {
                            Text(p.nick)
                            Text(
                                if (p.online) "在线" else "离线",
                                fontSize = 12.sp,
                                color = if (p.online) JitongBlue else SecondaryText,
                            )
                        }
                    }
                },
                navigationIcon = {
                    TextButton(onClick = { vm.backToFriends() }) { Text("‹", fontSize = 32.sp) }
                },
                actions = {
                    Box {
                        TextButton(onClick = { showTopMenu = true }) {
                            Text("＋", fontSize = 25.sp, color = JitongBlue)
                        }
                        DropdownMenu(
                            expanded = showTopMenu,
                            onDismissRequest = { showTopMenu = false },
                        ) {
                            DropdownMenuItem(
                                text = { Text("查找聊天内容") },
                                leadingIcon = { Text("⌕", color = JitongBlue, fontSize = 19.sp) },
                                onClick = {
                                    showTopMenu = false
                                    showPanel = false
                                    vm.openChatSearch()
                                },
                            )
                            DropdownMenuItem(
                                text = { Text("删除好友", color = Color(0xFFD93025)) },
                                leadingIcon = { Text("−", color = Color(0xFFD93025), fontSize = 20.sp) },
                                onClick = {
                                    showTopMenu = false
                                    showDeleteConfirm = true
                                },
                            )
                        }
                    }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .background(PageBackground)
                .imePadding(),
        ) {
            LazyColumn(
                state = listState,
                reverseLayout = true,
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
                contentPadding = PaddingValues(top = 8.dp, bottom = 22.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                items(displayMessages, key = { it.msgId }) { msg ->
                    MessageRow(msg, peerNick = p.nick, myNick = vm.myNick.collectAsStateWithLifecycle().value, myId = vm.myId, vm = vm)
                }
            }

            when (val state = aiReplyState) {
                AiReplyUiState.Idle -> Unit
                is AiReplyUiState.Loading -> {
                    Row(
                        Modifier.fillMaxWidth().background(Color.White)
                            .padding(horizontal = 14.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(10.dp))
                        Text("AI 正在分析最近的聊天…", modifier = Modifier.weight(1f), color = SecondaryText)
                        TextButton(onClick = vm::cancelAiReply) { Text("取消") }
                    }
                }
                is AiReplyUiState.Suggestions -> {
                    Column(
                        Modifier.fillMaxWidth().background(Color.White)
                            .padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(7.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("✦ AI 候选回复", color = JitongBlue, modifier = Modifier.weight(1f))
                            TextButton(onClick = vm::dismissAiReplies) { Text("收起") }
                        }
                        state.items.forEach { suggestion ->
                            Box(
                                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))
                                    .background(PaleBlue)
                                    .clickable {
                                        input = suggestion
                                        vm.dismissAiReplies()
                                    }
                                    .padding(horizontal = 14.dp, vertical = 11.dp),
                            ) { Text(suggestion, color = Color(0xFF1F2A44)) }
                        }
                        Text("点击候选内容只会填入输入框，不会自动发送", fontSize = 11.sp, color = SecondaryText)
                    }
                }
                is AiReplyUiState.Error -> {
                    Row(
                        Modifier.fillMaxWidth().background(Color.White)
                            .padding(horizontal = 14.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(state.message, modifier = Modifier.weight(1f), color = Color(0xFFD93025))
                        if (state.canRetry) {
                            TextButton(onClick = vm::retryAiReply) { Text("重试") }
                        }
                        TextButton(onClick = vm::dismissAiReplies) { Text("关闭") }
                    }
                }
            }

            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Color.White)
                    .padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier
                        .padding(end = 6.dp)
                        .size(40.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .background(PaleBlue)
                        .clickable { vm.requestAiReply() },
                    contentAlignment = Alignment.Center,
                ) { Text("AI", fontSize = 13.sp, color = JitongBlue) }
                OutlinedTextField(
                    value = input,
                    onValueChange = { input = it },
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("输入消息…") },
                    maxLines = 3,
                )
                // 「+」号入口（对齐 QQ/微信）：展开 相册/文件 面板
                Box(
                    Modifier
                        .padding(start = 8.dp)
                        .size(40.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color.White)
                        .clickable {
                            showPanel = !showPanel
                            if (showPanel) keyboard?.hide()
                        },
                    contentAlignment = Alignment.Center,
                ) { Text(if (showPanel) "×" else "＋", fontSize = 22.sp, color = JitongBlue) }
                Button(
                    onClick = {
                        vm.send(input)
                        input = ""
                    },
                    modifier = Modifier.padding(start = 8.dp),
                ) { Text("发送") }
            }

            // 「+」展开面板
            if (showPanel) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Color.White)
                        .padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(24.dp),
                ) {
                    PanelItem("相册") {
                        showPanel = false
                        pickImage.launch(
                            PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                        )
                    }
                    PanelItem("文件") {
                        showPanel = false
                        pickFile.launch(arrayOf("*/*"))
                    }
                }
            }
        }
    }
}

@Composable
private fun PanelItem(label: String, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier
                .size(60.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(Color.White)
                .clickable(onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Text(label.take(1), fontSize = 24.sp, color = JitongBlue)
        }
        Spacer(Modifier.height(4.dp))
        Text(label, fontSize = 12.sp, color = Color.Gray)
    }
}

/** 一条消息：头像 + 气泡（自己靠右绿色，对方靠左白色） */
@Composable
private fun MessageRow(msg: ChatMessage, peerNick: String, myNick: String, myId: Int, vm: MainViewModel) {
    val context = LocalContext.current
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (msg.fromMe) Arrangement.End else Arrangement.Start,
        verticalAlignment = Alignment.Top,
    ) {
        if (!msg.fromMe) {
            Avatar(id = msg.peerId, nick = peerNick, size = 40.dp)
            Spacer(Modifier.width(8.dp))
        }
        Column(
            horizontalAlignment = if (msg.fromMe) Alignment.End else Alignment.Start,
        ) {
            when (msg.kind) {
                MsgKind.TEXT -> Box(
                    Modifier
                        .widthIn(max = 260.dp)
                        .background(
                            if (msg.fromMe) PaleBlue else Color.White,
                            RoundedCornerShape(14.dp),
                        )
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                ) { Text(msg.text) }

                MsgKind.IMAGE -> ImageBubble(msg)

                // 文件消息气泡：图标 + 名称 + 大小 + 进度/下载/打开
                MsgKind.FILE -> FileBubble(msg, onDownload = { vm.downloadFile(msg) }, onOpen = {
                    msg.localPath?.let { openFile(context, it, msg.fileName) }
                }, onRetry = { vm.retryFile(msg) })
            }
            if (msg.fromMe) {
                Text(
                    when (msg.status) {
                        ChatMessage.Status.SENDING -> "发送中…"
                        ChatMessage.Status.DELIVERED -> "已送达"
                        ChatMessage.Status.OFFLINE_STORED -> "对方离线，已转存"
                        ChatMessage.Status.FAILED -> "发送失败，点击重试"
                        ChatMessage.Status.RECEIVED -> ""
                    },
                    fontSize = 10.sp,
                    color = Color.Gray,
                )
            }
        }
        if (msg.fromMe) {
            Spacer(Modifier.width(8.dp))
            Avatar(id = myId, nick = myNick, size = 40.dp)
        }
    }
}

/**
 * 固定尺寸文件卡片：左侧文件图标，右侧文件名及大小/操作状态。
 *
 * 本机存在有效文件时直接打开；没有本地副本但存在服务端 fileId 时允许重新下载。
 * 发送方在换设备、清缓存或原文件被删除后同样需要这条下载路径，不能只允许接收方下载。
 */
@Composable
private fun FileBubble(msg: ChatMessage, onDownload: () -> Unit, onOpen: () -> Unit, onRetry: () -> Unit) {
    val downloaded = msg.localPath?.let { path ->
        !path.endsWith(".part") &&
            (path.startsWith("content://") || java.io.File(path).isFile)
    } == true
    val downloading = msg.status == ChatMessage.Status.SENDING
    val failed = msg.fromMe && msg.status == ChatMessage.Status.FAILED
    val canDownload = msg.fileId.isNotBlank()
    val progress = msg.transferred.coerceIn(0, 100)
    val actionText = when {
        downloading -> "传输中 $progress%"
        failed -> "点击重试"
        downloaded -> "点击打开"
        canDownload -> "点击下载"
        else -> "文件不可用"
    }
    val actionColor = when {
        failed -> Color(0xFFD93025)
        downloaded || canDownload -> JitongBlue
        else -> Color.Gray
    }

    Row(
        Modifier
            .width(260.dp)
            .height(76.dp)
            .background(if (msg.fromMe) PaleBlue else Color.White, RoundedCornerShape(14.dp))
            .clickable {
                when {
                    failed -> onRetry()
                    downloaded -> onOpen()
                    canDownload && !downloading -> onDownload()
                    // 上传/下载中或缺少服务端 fileId：忽略点击。
                }
            }
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(48.dp)
                .background(Color.White.copy(alpha = 0.72f), RoundedCornerShape(8.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("📄", fontSize = 27.sp)
        }
        Spacer(Modifier.width(12.dp))
        Column(
            Modifier.weight(1f),
            verticalArrangement = Arrangement.Center,
        ) {
            Text(
                text = msg.fileName.ifBlank { "未命名文件" },
                fontSize = 14.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(5.dp))
            Text(
                text = "${humanSize(msg.fileSize)} · $actionText",
                fontSize = 11.sp,
                color = actionColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

private fun humanSize(b: Long): String = when {
    b >= 1 shl 20 -> "%.1f MB".format(b / 1048576.0)
    b >= 1 shl 10 -> "%.1f KB".format(b / 1024.0)
    else -> "$b B"
}
private fun openFile(context: android.content.Context, path: String, name: String) {
    runCatching {
        val uri = if (path.startsWith("content://")) {
            android.net.Uri.parse(path)
        } else {
            androidx.core.content.FileProvider.getUriForFile(
                context, "${context.packageName}.fileprovider", java.io.File(path),
            )
        }
        val mime = context.contentResolver.getType(uri)
            ?: java.net.URLConnection.guessContentTypeFromName(name)
            ?: "*/*"
        val intent = android.content.Intent(android.content.Intent.ACTION_VIEW)
            .setDataAndType(uri, mime)
            .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        context.startActivity(intent)
    }
        .onFailure { android.widget.Toast.makeText(context, "无可打开该文件的应用", android.widget.Toast.LENGTH_SHORT).show() }
}

@Composable
private fun ImageBubble(msg: ChatMessage) {
    if (!msg.localPath.isNullOrBlank()) {
        val ratio = if (msg.imgW > 0 && msg.imgH > 0) msg.imgW.toFloat() / msg.imgH else 1f
        AsyncImage(
            model = java.io.File(msg.localPath),
            contentDescription = "图片消息",
            contentScale = ContentScale.Crop,
            modifier = Modifier.widthIn(max = 220.dp)
                .aspectRatio(ratio.coerceIn(0.4f, 2.5f))
                .clip(RoundedCornerShape(8.dp)),
        )
        return
    }
    val bitmap = remember(msg.msgId) {
        msg.imageBytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
    }
    if (bitmap == null) {
        Text("[图片无法显示]", color = Color.Gray, fontSize = 12.sp)
        return
    }
    val ratio = if (msg.imgW > 0 && msg.imgH > 0) {
        msg.imgW.toFloat() / msg.imgH.toFloat()
    } else {
        bitmap.width.toFloat() / bitmap.height.toFloat()
    }
    Image(
        bitmap = bitmap,
        contentDescription = "图片消息",
        contentScale = ContentScale.Crop,
        modifier = Modifier
            .widthIn(max = 220.dp)
            .aspectRatio(ratio.coerceIn(0.4f, 2.5f))
            .clip(RoundedCornerShape(8.dp))
            .background(Color.White),
    )
}
