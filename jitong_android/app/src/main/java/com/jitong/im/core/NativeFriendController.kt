package com.jitong.im.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/** 好友页面薄适配器：只观察 Native 快照并转发用户意图。 */
class NativeFriendController(
    private val sdk: JitongSdk,
    scope: CoroutineScope,
) {
    private val _friends=MutableStateFlow<List<JitongSdk.NativeFriend>>(emptyList())
    val friends:StateFlow<List<JitongSdk.NativeFriend>> = _friends
    private val _requests=MutableStateFlow<List<JitongSdk.NativeFriendRequest>>(emptyList())
    val requests:StateFlow<List<JitongSdk.NativeFriendRequest>> = _requests

    init {
        scope.launch(Dispatchers.IO) {
            sdk.dataVersions.map { it[JitongSdk.DataDomain.Friends]?:0L }
                .distinctUntilChanged().collect {
                    _friends.value=sdk.loadFriends().orEmpty()
                        .sortedWith(compareBy({!it.online},{it.friendId}))
                    _requests.value=sdk.loadFriendRequests().orEmpty()
                }
        }
    }

    fun refresh()=sdk.requestFriendRequests()
    fun add(nick:String)=sdk.sendAddFriend(nick)
    fun answer(request:JitongSdk.NativeFriendRequest,agree:Boolean)=
        sdk.answerFriend(request.fromUserId,request.message,agree)
    fun delete(friendId:Long)=sdk.deleteFriend(friendId)
}
