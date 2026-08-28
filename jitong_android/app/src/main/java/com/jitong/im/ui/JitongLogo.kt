package com.jitong.im.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.jitong.im.R

@Composable
fun JitongLogo(size: Dp = 40.dp, modifier: Modifier = Modifier) {
    Image(
        painter = painterResource(R.drawable.jitong_logo),
        contentDescription = "即通 Logo",
        contentScale = ContentScale.Fit,
        modifier = modifier.size(size),
    )
}
