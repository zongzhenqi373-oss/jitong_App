package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.net.Protocol
import com.jitong.im.net.ImClient
import com.jitong.im.ui.theme.*
import java.text.SimpleDateFormat
import java.util.*

private enum class HomeTab(val label: String, val glyph: String) {
    Messages("消息", "●"), Contacts("联系人", "♟"), Me("我的", "○")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FriendListScreen(vm: MainViewModel) {
    val myNick by vm.myNick.collectAsStateWithLifecycle()
    val myFeeling by vm.myFeeling.collectAsStateWithLifecycle()
    val friends by vm.friends.collectAsStateWithLifecycle()
    val conversations by vm.conversations.collectAsStateWithLifecycle()
    val results by vm.searchResults.collectAsStateWithLifecycle()
    val friendRequests by vm.friendRequests.collectAsStateWithLifecycle()
    val incomingRequestCount = friendRequests.count{ it.targetId == vm.myId }
    var tab by rememberSaveable { mutableStateOf(HomeTab.Messages) }
    var query by rememberSaveable { mutableStateOf("") }
    var showAddFriend by remember { mutableStateOf(false) }
    var showMenu by remember { mutableStateOf(false) }
    var showNewFriends by remember { mutableStateOf(false) }

    Scaffold(
        containerColor = PageBackground,
        bottomBar = { JitongBottomBar(tab) { tab = it; query = ""; vm.search("") } },
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).statusBarsPadding()) {
            when (tab) {
                HomeTab.Messages -> MessagesTab(
                    myNick, friends, conversations, results, query,
                    onQuery = { query = it; vm.search(it) },
                    onOpen = vm::openChat,
                    onAddFriend = { showAddFriend = true },
                    onMenu = { showMenu = true },
                )
                HomeTab.Contacts -> ContactsTab(
                    friends = friends,
                    requestCount = incomingRequestCount,
                    onOpen = vm::openChat,
                    onAddFriend = { showAddFriend = true },
                    onNewFriends = {
                        showNewFriends = true
                        vm.loadFriendRequests()
                    },
                    onGroup = { vm.notify("群聊功能即将上线") },
                )
                HomeTab.Me -> MeTab(vm.myId, myNick, myFeeling, vm::notify, vm::logout)
            }
        }
    }

    if (showMenu) {
        QuickActionDialog(
            onDismiss = { showMenu = false },
            onAddFriend = { showMenu = false; showAddFriend = true },
            onSearch = { showMenu = false },
        )
    }
    if (showAddFriend) AddFriendDialog(
        onDismiss = { showAddFriend = false },
        onSend = { vm.sendAddFriendRequest(it); showAddFriend = false },
    )
    if (showNewFriends) {
        NewFriendsDialog(
            myId = vm.myId,
            requests = friendRequests,
            onDismiss = { showNewFriends = false },
            onAccept = { vm.respondFriendRequest(it, Protocol.ADD_FRIEND_AGREE) },
            onReject = { vm.respondFriendRequest(it, Protocol.ADD_FRIEND_REJECT) },
            onAdd = { showNewFriends = false; showAddFriend = true },
        )
    }
}

@Composable
private fun MessagesTab(
    myNick: String, friends: List<Friend>, conversations: Map<Int, com.jitong.im.data.db.ConversationEntity>,
    results: List<com.jitong.im.data.db.MessageEntity>, query: String,
    onQuery: (String) -> Unit, onOpen: (Friend) -> Unit, onAddFriend: () -> Unit, onMenu: () -> Unit,
) {
    HomeHeader(myNick.ifBlank { "即通用户" }, onMenu)
    SearchBox(query, onQuery, "搜索聊天记录")
    Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        QuickAction("♙", "添加好友", Modifier.weight(1f), onAddFriend)
        QuickAction("●", "发起聊天", Modifier.weight(1f)) {
            friends.firstOrNull()?.let(onOpen)
        }
    }
    Text("最近消息", Modifier.padding(horizontal = 18.dp, vertical = 6.dp), fontWeight = FontWeight.SemiBold)
    Surface(Modifier.fillMaxSize().padding(horizontal = 12.dp), shape = RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp)) {
        when {
            query.isNotBlank() -> LazyColumn {
                items(results, key = { it.msgId }) { m ->
                    friends.firstOrNull { it.id == m.peerId }?.let { SearchResultRow(it, m.content.orEmpty(), m.ts) { onOpen(it) } }
                }
            }
            friends.isEmpty() -> EmptyState("还没有消息", "点击上方“添加好友”开始聊天")
            else -> LazyColumn {
                items(friends, key = { it.id }) { friend ->
                    ConversationRow(friend, conversations[friend.id]) { onOpen(friend) }
                    HorizontalDivider(Modifier.padding(start = 76.dp), thickness = 0.5.dp, color = Color(0xFFEDF0F5))
                }
            }
        }
    }
}

@Composable
private fun ContactsTab(
    friends: List<Friend>, requestCount: Int, onOpen: (Friend) -> Unit, onAddFriend: () -> Unit,
    onNewFriends: () -> Unit, onGroup: () -> Unit,
) {
    HomeHeader("联系人", onAddFriend)
    var query by rememberSaveable { mutableStateOf("") }
    SearchBox(query, { query = it }, "搜索联系人")
    LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Surface(shape = RoundedCornerShape(16.dp)) {
                Column {
                    ContactAction("＋", "新的朋友", "好友申请", badgeCount = requestCount, onClick = onNewFriends)
                    HorizontalDivider(Modifier.padding(start = 70.dp), color = Color(0xFFEDF0F5))
                    ContactAction("♟", "群聊", "创建和管理群聊", onClick = onGroup)
                }
            }
        }
        val filtered = friends.filter { query.isBlank() || it.nick.contains(query, ignoreCase = true) }
        if (filtered.isEmpty()) item { EmptyState("暂无联系人", "添加好友后会显示在这里") }
        else groupedContacts(filtered).forEach { (letter, group) ->
            item { Text(letter, Modifier.padding(start = 8.dp, top = 8.dp), color = SecondaryText, fontSize = 12.sp, fontWeight = FontWeight.Bold) }
            item {
                Surface(shape = RoundedCornerShape(16.dp)) {
                    Column {
                        group.forEachIndexed { index, friend ->
                            ContactRow(friend) { onOpen(friend) }
                            if (index != group.lastIndex) HorizontalDivider(Modifier.padding(start = 70.dp), color = Color(0xFFEDF0F5))
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun MeTab(id: Int, nick: String, feeling: String, notify: (String) -> Unit, logout: () -> Unit) {
    Text("我的", Modifier.padding(horizontal = 18.dp, vertical = 16.dp), style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
    LazyColumn(Modifier.fillMaxSize().padding(horizontal = 14.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            Surface(shape = RoundedCornerShape(20.dp), shadowElevation = 1.dp) {
                Row(Modifier.fillMaxWidth().padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
                    Avatar(id, nick, 68.dp)
                    Spacer(Modifier.width(14.dp))
                    Column(Modifier.weight(1f)) {
                        Text(nick.ifBlank { "即通用户" }, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                        Text("即通号：JT${id.toString().padStart(4, '0')}", color = SecondaryText, fontSize = 13.sp)
                        Text(feeling.ifBlank { "保持热爱，奔赴山海" }, color = SecondaryText, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    Text("编辑资料", color = JitongBlue, fontSize = 13.sp, modifier = Modifier.clickable { notify("资料编辑功能即将上线") })
                }
            }
        }
        item {
            Surface(shape = RoundedCornerShape(18.dp)) {
                Column {
                    SettingsRow("○", "个人资料") { notify("资料编辑功能即将上线") }
                    SettingsRow("◇", "账号与安全") { notify("账号安全功能即将上线") }
                    SettingsRow("♧", "消息通知") { notify("通知设置功能即将上线") }
                    SettingsRow("ⓘ", "关于即通") { notify("即通 Android 0.5.0") }
                }
            }
        }
        item {
            OutlinedButton(
                onClick = logout,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                border = androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = .55f)),
            ) { Text("退出登录") }
        }
    }
}

@Composable private fun HomeHeader(title: String, onAction: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 18.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
        JitongLogo(34.dp)
        Spacer(Modifier.width(8.dp))
        Text(title, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Spacer(Modifier.weight(1f))
        Box(Modifier.size(38.dp).clip(CircleShape).background(PaleBlue).clickable(onClick = onAction), contentAlignment = Alignment.Center) {
            Text("＋", color = JitongBlue, fontSize = 24.sp)
        }
    }
}

@Composable private fun SearchBox(value: String, onChange: (String) -> Unit, hint: String) {
    OutlinedTextField(
        value, onChange, placeholder = { Text("⌕  $hint", fontSize = 14.sp, color = SecondaryText) }, singleLine = true,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp), shape = RoundedCornerShape(15.dp),
        colors = OutlinedTextFieldDefaults.colors(unfocusedBorderColor = Color.Transparent, focusedBorderColor = JitongBlue, unfocusedContainerColor = Color.White, focusedContainerColor = Color.White),
    )
}

@Composable private fun QuickAction(icon: String, label: String, modifier: Modifier, onClick: () -> Unit) {
    Surface(modifier.clickable(onClick = onClick), shape = RoundedCornerShape(16.dp), color = Color.White) {
        Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
            Text(icon, color = JitongBlue, fontSize = 20.sp); Spacer(Modifier.width(8.dp)); Text(label, fontWeight = FontWeight.Medium)
        }
    }
}

@Composable private fun ConversationRow(friend: Friend, conv: com.jitong.im.data.db.ConversationEntity?, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 12.dp, vertical = 11.dp), verticalAlignment = Alignment.CenterVertically) {
        OnlineAvatar(friend, 52.dp); Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(friend.nick, fontWeight = FontWeight.SemiBold)
                conv?.let { Text(formatTs(it.lastTs), color = SecondaryText, fontSize = 11.sp) }
            }
            Spacer(Modifier.height(4.dp))
            Text(conv?.lastMsg ?: if (friend.online) friend.feeling.ifBlank { "在线" } else "离线", color = SecondaryText, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        val unread = conv?.unread ?: 0
        if (unread > 0) Badge(containerColor = JitongBlue) { Text(if (unread > 99) "99+" else "$unread") }
    }
}

@Composable private fun OnlineAvatar(friend: Friend, size: androidx.compose.ui.unit.Dp) {
    Box { Avatar(friend.id, friend.nick, size); if (friend.online) Box(Modifier.size(12.dp).align(Alignment.BottomEnd).background(Color.White, CircleShape).padding(2.dp).background(JitongBlue, CircleShape)) }
}

@Composable private fun ContactAction(icon: String, title: String, subtitle: String, onClick: () -> Unit, badgeCount: Int = 0) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(42.dp).background(PaleBlue, RoundedCornerShape(13.dp)), contentAlignment = Alignment.Center) { Text(icon, color = JitongBlue, fontSize = 21.sp) }
        Spacer(Modifier.width(12.dp));
        Column(Modifier.weight(1f)) { Text(title, fontWeight = FontWeight.SemiBold); Text(subtitle, color = SecondaryText, fontSize = 12.sp) }
        if (badgeCount > 0) {
            Badge(containerColor = Color(0xFFF04444)) { Text(if (badgeCount > 99) "99+" else badgeCount.toString()) }
        } else {
            Text("›", color = SecondaryText, fontSize = 22.sp)
        }
    }
}

@Composable private fun ContactRow(friend: Friend, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
        OnlineAvatar(friend, 44.dp); Spacer(Modifier.width(12.dp)); Text(friend.nick, Modifier.weight(1f), fontWeight = FontWeight.Medium); Text(if (friend.online) "在线" else "", color = JitongBlue, fontSize = 12.sp)
    }
}

private fun groupedContacts(friends: List<Friend>): Map<String, List<Friend>> = friends.sortedBy { it.nick }.groupBy {
    it.nick.firstOrNull()?.uppercaseChar()?.toString() ?: "#"
}

@Composable private fun SettingsRow(icon: String, label: String, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 15.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(icon, color = JitongBlue, fontSize = 19.sp); Spacer(Modifier.width(14.dp)); Text(label, Modifier.weight(1f)); Text("›", color = SecondaryText, fontSize = 22.sp)
    }
}

@Composable private fun JitongBottomBar(tab: HomeTab, onSelect: (HomeTab) -> Unit) {
    NavigationBar(containerColor = Color.White, tonalElevation = 3.dp) {
        HomeTab.entries.forEach { item ->
            NavigationBarItem(
                selected = tab == item, onClick = { onSelect(item) },
                icon = { Text(item.glyph, color = if (tab == item) JitongBlue else SecondaryText, fontSize = 18.sp) },
                label = { Text(item.label) },
                colors = NavigationBarItemDefaults.colors(selectedIconColor = JitongBlue, selectedTextColor = JitongBlue, indicatorColor = PaleBlue),
            )
        }
    }
}

@Composable
private fun NewFriendsDialog(
    myId: Int,
    requests: List<ImClient.Event.FriendRequestItem>,
    onDismiss: () -> Unit,
    onAccept: (ImClient.Event.FriendRequestItem) -> Unit,
    onReject: (ImClient.Event.FriendRequestItem) -> Unit,
    onAdd: () -> Unit,
) {
    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(24.dp), color = Color.White, shadowElevation = 12.dp) {
            Column(Modifier.fillMaxWidth().heightIn(max = 580.dp).padding(20.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("新的朋友", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onAdd) { Text("添加好友") }
                    TextButton(onClick = onDismiss) { Text("关闭") }
                }
                Text("收到的申请和等待对方验证的申请", color = SecondaryText, fontSize = 12.sp)
                Spacer(Modifier.height(12.dp))
                if (requests.isEmpty()) {
                    Box(Modifier.fillMaxWidth().height(180.dp), contentAlignment = Alignment.Center) {
                        Text("暂无待处理的好友申请", color = SecondaryText)
                    }
                } else {
                    LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        items(requests, key = { "${it.requesterId}:${it.targetId}" }) { request ->
                            val incoming = request.targetId == myId
                            val peerId = if (incoming) request.requesterId else request.targetId
                            val peerNick = if (incoming) request.requesterNick else request.targetNick
                            Surface(color = Color(0xFFF7F9FC), shape = RoundedCornerShape(16.dp)) {
                                Row(
                                    Modifier.fillMaxWidth().padding(12.dp),
                                    verticalAlignment = Alignment.CenterVertically,
                                ) {
                                    Avatar(peerId, peerNick, 44.dp)
                                    Spacer(Modifier.width(10.dp))
                                    Column(Modifier.weight(1f)) {
                                        Text(peerNick.ifBlank { "即通用户$peerId" }, fontWeight = FontWeight.SemiBold)
                                        Text(
                                            if (incoming) "请求添加你为好友" else "等待对方验证",
                                            color = SecondaryText,
                                            fontSize = 12.sp,
                                        )
                                    }
                                    if (incoming) {
                                        TextButton(onClick = { onReject(request) }) { Text("拒绝", color = SecondaryText) }
                                        Button(onClick = { onAccept(request) }, shape = RoundedCornerShape(12.dp)) { Text("同意") }
                                    } else {
                                        Text("等待中", color = JitongBlue, fontSize = 12.sp)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable private fun AddFriendDialog(onDismiss: () -> Unit, onSend: (String) -> Unit) {
    var nick by rememberSaveable { mutableStateOf("") }
    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(24.dp), color = Color.White, shadowElevation = 12.dp) {
            Column(Modifier.fillMaxWidth().padding(22.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(42.dp).background(PaleBlue, RoundedCornerShape(13.dp)), contentAlignment = Alignment.Center) {
                        Text("＋", color = JitongBlue, fontSize = 24.sp)
                    }
                    Spacer(Modifier.width(12.dp))
                    Column { Text("添加好友", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold); Text("输入对方昵称发送好友申请", color = SecondaryText, fontSize = 12.sp) }
                }
                Spacer(Modifier.height(20.dp))
                OutlinedTextField(
                    nick, { nick = it }, placeholder = { Text("输入对方昵称") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(14.dp),
                    colors = OutlinedTextFieldDefaults.colors(focusedContainerColor = Color(0xFFF8FAFD), unfocusedContainerColor = Color(0xFFF8FAFD)),
                )
                Spacer(Modifier.height(20.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedButton(onClick = onDismiss, Modifier.weight(1f), shape = RoundedCornerShape(13.dp)) { Text("取消") }
                    Button(onClick = { if (nick.isNotBlank()) onSend(nick.trim()) }, enabled = nick.isNotBlank(), modifier = Modifier.weight(1f), shape = RoundedCornerShape(13.dp)) { Text("发送申请") }
                }
            }
        }
    }
}

@Composable private fun QuickActionDialog(onDismiss: () -> Unit, onAddFriend: () -> Unit, onSearch: () -> Unit) {
    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(24.dp), color = Color.White, shadowElevation = 12.dp) {
            Column(Modifier.fillMaxWidth().padding(22.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    JitongLogo(42.dp)
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text("快捷操作", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                        Text("选择你要进行的操作", color = SecondaryText, fontSize = 12.sp)
                    }
                    Box(Modifier.size(32.dp).clip(CircleShape).background(Color(0xFFF1F4F8)).clickable(onClick = onDismiss), contentAlignment = Alignment.Center) { Text("×", color = SecondaryText, fontSize = 20.sp) }
                }
                Spacer(Modifier.height(20.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    QuickDialogCard("＋", "添加好友", "通过昵称查找", Modifier.weight(1f), onAddFriend)
                    QuickDialogCard("⌕", "搜索消息", "查找聊天记录", Modifier.weight(1f), onSearch)
                }
            }
        }
    }
}

@Composable private fun QuickDialogCard(icon: String, title: String, subtitle: String, modifier: Modifier, onClick: () -> Unit) {
    Column(
        modifier.clip(RoundedCornerShape(16.dp)).background(Color(0xFFF5F9FF)).clickable(onClick = onClick).padding(16.dp),
    ) {
        Box(Modifier.size(38.dp).background(PaleBlue, RoundedCornerShape(12.dp)), contentAlignment = Alignment.Center) { Text(icon, color = JitongBlue, fontSize = 22.sp) }
        Spacer(Modifier.height(12.dp)); Text(title, fontWeight = FontWeight.SemiBold); Text(subtitle, color = SecondaryText, fontSize = 11.sp)
    }
}

@Composable private fun EmptyState(title: String, subtitle: String) {
    Column(Modifier.fillMaxWidth().padding(48.dp), horizontalAlignment = Alignment.CenterHorizontally) { JitongLogo(58.dp); Spacer(Modifier.height(12.dp)); Text(title, fontWeight = FontWeight.SemiBold); Text(subtitle, color = SecondaryText, fontSize = 13.sp) }
}

@Composable private fun SearchResultRow(friend: Friend, content: String, ts: Long, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
        Avatar(friend.id, friend.nick, 44.dp); Spacer(Modifier.width(12.dp)); Column(Modifier.weight(1f)) { Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text(friend.nick, fontWeight = FontWeight.SemiBold); Text(formatTs(ts), color = SecondaryText, fontSize = 11.sp) }; Text(content, color = SecondaryText, maxLines = 1, overflow = TextOverflow.Ellipsis) }
    }
}

private fun formatTs(ts: Long): String {
    val sameDay = SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date(ts)) == SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date())
    return SimpleDateFormat(if (sameDay) "HH:mm" else "MM-dd", Locale.getDefault()).format(Date(ts))
}
