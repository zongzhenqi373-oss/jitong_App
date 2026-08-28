package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.ui.theme.JitongBlue
import com.jitong.im.ui.theme.PageBackground
import com.jitong.im.ui.theme.SecondaryText
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 独立的单聊记录查找页：空关键词显示当前已漫游到本地的记录，输入后查询 Room FTS。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatSearchScreen(vm: MainViewModel) {
    val peer by vm.chatPeer.collectAsStateWithLifecycle()
    val allMessages by vm.messages.collectAsStateWithLifecycle()
    val searchResults by vm.conversationSearchResults.collectAsStateWithLifecycle()
    val myNick by vm.myNick.collectAsStateWithLifecycle()
    val currentPeer = peer ?: return
    val history = allMessages[currentPeer.id].orEmpty().sortedByDescending { it.ts }
    var query by rememberSaveable(currentPeer.id) { mutableStateOf("") }
    val focusRequester = remember { FocusRequester() }

    LaunchedEffect(Unit) { focusRequester.requestFocus() }

    Scaffold(
        containerColor = PageBackground,
        topBar = {
            TopAppBar(
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Color.White),
                navigationIcon = {
                    TextButton(onClick = { vm.backToChat() }) { Text("‹", fontSize = 32.sp) }
                },
                title = { Text("查找聊天内容", fontWeight = FontWeight.SemiBold) },
            )
        },
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            Box(Modifier.fillMaxWidth().background(Color.White).padding(horizontal = 16.dp, vertical = 12.dp)) {
                OutlinedTextField(
                    value = query,
                    onValueChange = {
                        query = it
                        vm.searchCurrentConversation(it)
                    },
                    modifier = Modifier.fillMaxWidth().focusRequester(focusRequester),
                    placeholder = { Text("搜索聊天记录", color = SecondaryText) },
                    leadingIcon = { Text("⌕", color = SecondaryText, fontSize = 20.sp) },
                    singleLine = true,
                    shape = RoundedCornerShape(16.dp),
                )
            }

            val empty = if (query.isBlank()) history.isEmpty() else searchResults.isEmpty()
            if (empty) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        if (query.isBlank()) "暂无聊天记录" else "没有找到相关聊天记录",
                        color = SecondaryText,
                    )
                }
            } else {
                LazyColumn(Modifier.fillMaxSize().background(Color.White)) {
                    if (query.isBlank()) {
                        items(history, key = { it.msgId }) { message ->
                            ChatHistorySearchRow(
                                avatarId = if (message.fromMe) vm.myId else currentPeer.id,
                                senderName = if (message.fromMe) myNick.ifBlank { "我" } else currentPeer.nick,
                                content = when (message.kind) {
                                    MsgKind.TEXT -> message.text
                                    MsgKind.IMAGE -> "[图片]"
                                    MsgKind.FILE -> "[文件] ${message.fileName}"
                                },
                                ts = message.ts,
                                onClick = { vm.returnToChatAt(message.msgId) },
                            )
                        }
                    } else {
                        items(searchResults, key = { it.msgId }) { message ->
                            ChatHistorySearchRow(
                                avatarId = if (message.fromMe) vm.myId else currentPeer.id,
                                senderName = if (message.fromMe) myNick.ifBlank { "我" } else currentPeer.nick,
                                content = message.content.orEmpty(),
                                ts = message.ts,
                                onClick = { vm.returnToChatAt(message.msgId) },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ChatHistorySearchRow(
    avatarId: Int,
    senderName: String,
    content: String,
    ts: Long,
    onClick: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().background(Color.White).clickable(onClick = onClick)) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Avatar(id = avatarId, nick = senderName, size = 44.dp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text(senderName, color = SecondaryText, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    Text(formatSearchTime(ts), color = SecondaryText, fontSize = 12.sp)
                }
                Text(content, maxLines = 3, overflow = TextOverflow.Ellipsis, fontSize = 16.sp)
                Text("定位到聊天位置", color = JitongBlue, fontSize = 12.sp)
            }
        }
        HorizontalDivider(
            modifier = Modifier.padding(start = 72.dp),
            thickness = 0.5.dp,
            color = Color(0xFFEDF0F5),
        )
    }
}

private fun formatSearchTime(ts: Long): String =
    SimpleDateFormat("yyyy年M月d日 HH:mm", Locale.getDefault()).format(Date(ts))
