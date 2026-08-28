package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** 首字符头像：无头像资源时用昵称首字符 + id 派生底色（对齐 QQ/微信默认头像观感） */
@Composable
fun Avatar(id: Int, nick: String, size: Dp = 48.dp) {
    val palette = listOf(
        Color(0xFF5B8DEF), Color(0xFFE06C75), Color(0xFF56B6C2), Color(0xFFD19A66),
        Color(0xFF98C379), Color(0xFFC678DD), Color(0xFF61AFEF), Color(0xFFBE8C66),
    )
    val bg = palette[(id % palette.size + palette.size) % palette.size]
    val initial = nick.firstOrNull()?.toString() ?: "?"
    Box(
        modifier = Modifier
            .size(size)
            .background(bg, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text(initial, color = Color.White, fontSize = (size.value * 0.42).sp)
    }
}
