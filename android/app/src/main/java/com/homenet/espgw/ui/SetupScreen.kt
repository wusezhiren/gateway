package com.homenet.espgw.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SetupScreen(hasGateway: Boolean, onProvision: () -> Unit, onBind: () -> Unit, onBack: () -> Unit) {
    Scaffold(topBar = {
        TopAppBar(
            title = { Text("添加网关") },
            navigationIcon = {
                if (hasGateway) {
                    IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "返回") }
                }
            },
        )
    }) { pad ->
        Column(
            Modifier.fillMaxSize().padding(pad).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            SectionCard("新网关（还没联网）") {
                Text(
                    "长按网关上的 REC 键 3 秒，屏幕出现二维码后，用 App 连接网关热点并选择家里的 Wi-Fi。",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Button(onClick = onProvision, modifier = Modifier.fillMaxWidth().padding(top = 12.dp)) {
                    Text("开始配网")
                }
            }
            SectionCard("已联网的网关") {
                Text(
                    "手机和网关在同一个 Wi-Fi 下，App 自动查找网关，在网关屏幕上点“允许”后完成绑定。",
                    style = MaterialTheme.typography.bodyMedium,
                )
                OutlinedButton(onClick = onBind, modifier = Modifier.fillMaxWidth().padding(top = 12.dp)) {
                    Text("查找并绑定")
                }
            }
        }
    }
}
