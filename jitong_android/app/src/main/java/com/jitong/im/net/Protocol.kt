package com.jitong.im.net

/**
 * 协议号与结果码常量。
 * 必须与 client_core/include/client_core/Protocol.h（及服务端）保持一致。
 * 线格式：[4B 大端包长(含协议号)][4B 小端协议号][pb payload]
 */
object Protocol {
    //tcp连接端口
    const val TCP_PORT          = 24563
    const val HTTPS_FILE_PORT   = 24564

    const val DEF_BASE          = 1000
    const val REGISTER_RQ       = DEF_BASE + 0
    const val REGISTER_RS       = DEF_BASE + 1
    const val LOGIN_RQ          = DEF_BASE + 2
    const val LOGIN_RS          = DEF_BASE + 3
    const val FRIEND_INFO       = DEF_BASE + 4
    const val CHAT_INFO_RQ      = DEF_BASE + 5
    const val CHAT_INFO_RS      = DEF_BASE + 6
    const val ADD_FRIEND_RQ     = DEF_BASE + 7
    const val ADD_FRIEND_RS     = DEF_BASE + 8
    const val FRIEND_OFFLINE    = DEF_BASE + 9
    const val HEARTBEAT_RQ      = DEF_BASE + 10
    const val HEARTBEAT_RS      = DEF_BASE + 11
    const val KICKED_OFFLINE    = DEF_BASE + 12
    const val ROAM_CONV_RQ      = DEF_BASE + 13
    const val ROAM_CONV_RS      = DEF_BASE + 14
    const val ROAM_MSG_RQ       = DEF_BASE + 15
    const val ROAM_MSG_RS       = DEF_BASE + 16
    // 1017..1022 已废弃并保留，媒体字节统一走独立 HTTPS 文件服务。
    const val TOKEN_LOGIN_RQ    = DEF_BASE + 23
    const val TOKEN_LOGIN_RS    = DEF_BASE + 24
    const val TOKEN_REFRESH_RQ  = DEF_BASE + 25
    const val TOKEN_REFRESH_RS  = DEF_BASE + 26
    const val LOGOUT_RQ         = DEF_BASE + 27
    const val LOGOUT_RS         = DEF_BASE + 28
    const val AI_REPLY_RQ       = DEF_BASE + 29
    const val AI_REPLY_RS       = DEF_BASE + 30
    const val AI_CANCEL_RQ      = DEF_BASE + 31
    const val DELETE_FRIEND_RQ  = DEF_BASE + 32
    const val DELETE_FRIEND_RS  = DEF_BASE + 33
    const val FRIEND_REQUEST_LIST_RQ = DEF_BASE + 34
    const val FRIEND_REQUEST_LIST_RS = DEF_BASE + 35


    /** 单包最大长度（含 4B 协议号），防恶意超大包 OOM */
    const val MAX_PACK_LEN = 10 * 1024 * 1024

    // 结果码
    const val REGISTER_SUCC = 1
    const val REGISTER_NICK_EXIT = 2
    const val REGISTER_TEL_EXIT = 3

    const val LOGIN_SUCCESS = 0
    const val LOGIN_NOTEXIT = 1
    const val LOGIN_PASSERROR = 2
    const val REFRESH_TOKEN_SUCCESS = 0
    const val REFRESH_TOKEN_FAIL = 1
    const val LOGOUT_SUCCESS = 0
    const val LOGOUT_FAIL = 1

    const val STATUS_ONLINE = 0
    const val STATUS_OFFLINE = 1

    const val CHAT_RESULT_SUCC = 0 // 已送达（对方在线）
    const val CHAT_RESULT_FAIL = 1 // 对方离线，已转存
    const val CHAT_RESULT_NOT_FRIEND = 2  //对方不是你的好友
    const val CHAT_RESULT_SERVER_ERROR = 3   //消息假成功

    const val ADD_FRIEND_AGREE = 0
    const val ADD_FRIEND_REJECT = 1
    const val ADD_FRIEND_OFFLINE = 2
    const val ADD_FRIEND_NOTEXIT = 3
    const val ADD_FRIEND_SELF = 4
    const val ADD_FRIEND_ALREADY = 5
    const val ADD_FRIEND_PENDING = 6
    const val ADD_FRIEND_DB_ERROR = 7

    const val DELETE_FRIEND_SUCCESS = 0
    const val DELETE_FRIEND_DB_ERROR = 1
    const val DELETE_FRIEND_NOT_FRIEND = 2
    const val DELETE_FRIEND_INVALID = 3


    const val FILE_MAX_SIZE = 100L * 1024 * 1024
}
