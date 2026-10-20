package com.homenet.espgw.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/** 与网关屏幕一致的蓝色主色；Android 12+ 跟随系统取色 */
private val Accent = Color(0xFF3C8CFF)

val OkGreen = Color(0xFF2BB673)
val WarnOrange = Color(0xFFF0A020)

@Composable
fun EspGwTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val ctx = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> darkColorScheme(primary = Accent)
        else -> lightColorScheme(primary = Accent)
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
