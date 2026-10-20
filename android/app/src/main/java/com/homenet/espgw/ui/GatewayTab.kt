package com.homenet.espgw.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.homenet.espgw.ui.theme.OkGreen

@Composable
fun GatewayTab(vm: HomeViewModel, onAddGateway: () -> Unit) {
    val info = vm.info
    val gw = vm.gateway
    var confirmForget by remember { mutableStateOf(false) }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        SectionCard("网络") {
            InfoRow("Wi-Fi", info?.wifiSsid ?: "—")
            InfoRow(
                "状态",
                when {
                    info == null -> "—"
                    info.wifiConnected -> "已连接"
                    else -> "未连接"
                },
                if (info?.wifiConnected == true) OkGreen else MaterialTheme.colorScheme.error,
            )
            InfoRow("IP", gw?.ip ?: "—")
            InfoRow("信号", info?.let { rssiText(it.rssi) } ?: "—")
            InfoRow("信道", info?.channel?.toString() ?: "—")
        }
        SectionCard("网关") {
            InfoRow("名称", gw?.name ?: "—")
            InfoRow("MAC", gw?.mac ?: "—")
            InfoRow("固件版本", info?.let { "v${it.version}（${it.partition}）" } ?: "—")
            InfoRow("运行时间", info?.let { formatUptime(it.uptimeS) } ?: "—")
            InfoRow("节点", info?.let { "在线 ${it.nodesOnline} / 共 ${it.nodesTotal}" } ?: "—")
            InfoRow("IDF", info?.idf ?: "—")
        }
        SectionCard("手机") {
            Text(
                "在外网使用：家里放一台常开设备（如树莓派）做 Tailscale 子网路由，手机装 Tailscale 后即可按上面的 IP 访问网关。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = onAddGateway, modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
                Text("重新配网 / 更换网关")
            }
            OutlinedButton(onClick = { confirmForget = true }, modifier = Modifier.fillMaxWidth()) {
                Text("从手机移除此网关", color = MaterialTheme.colorScheme.error)
            }
        }
    }
    if (confirmForget) {
        ConfirmDialog(
            title = "移除网关",
            text = "只删除手机上保存的地址和凭据，网关和节点不受影响。之后可以重新绑定。",
            confirm = "移除",
            onConfirm = { vm.saveGateway(null) },
            onDismiss = { confirmForget = false },
        )
    }
}
