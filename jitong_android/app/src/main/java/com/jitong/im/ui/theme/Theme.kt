package com.jitong.im.ui.theme

import android.app.Activity
import android.graphics.Color as AndroidColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

val JitongBlue = Color(0xFF1677FF)
val JitongBlueDark = Color(0xFF125FD1)
val PaleBlue = Color(0xFFEAF3FF)
val PageBackground = Color(0xFFF4F7FB)
val PrimaryText = Color(0xFF172033)
val SecondaryText = Color(0xFF778195)

private val JitongColorScheme = lightColorScheme(
    primary = JitongBlue,
    onPrimary = Color.White,
    primaryContainer = PaleBlue,
    onPrimaryContainer = JitongBlueDark,
    secondary = Color(0xFF536D9C),
    surface = Color.White,
    background = PageBackground,
    onSurface = PrimaryText,
    outline = Color(0xFFDCE4EF),
    error = Color(0xFFE5484D),
)

@Composable
fun JitongTheme(content: @Composable () -> Unit) {
    val view = LocalView.current
    if (!view.isInEditMode) {
        val window = (view.context as Activity).window
        window.statusBarColor = AndroidColor.TRANSPARENT
        window.navigationBarColor = AndroidColor.WHITE
        WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = true
        WindowCompat.getInsetsController(window, view).isAppearanceLightNavigationBars = true
    }
    MaterialTheme(colorScheme = JitongColorScheme, typography = Typography(), content = content)
}
