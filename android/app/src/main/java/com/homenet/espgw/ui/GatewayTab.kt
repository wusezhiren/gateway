package com.homenet.espgw.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.TextButton
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import com.homenet.espgw.data.CloudConfig
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
fun GatewayTab(vm: HomeViewModel, upd: UpdateViewModel, onAddGateway: () -> Unit) {
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
        CloudCard(vm)
        SectionCard("手机") {
            InfoRow("App 版本", "v${upd.currentVersion}")
            OutlinedButton(
                onClick = { upd.check(manual = true) },
                enabled = !upd.checking,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
            ) { Text(if (upd.checking) "正在检查…" else "检查更新") }
            OutlinedButton(onClick = onAddGateway, modifier = Modifier.fillMaxWidth()) {
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

@Composable
private fun CloudCard(vm: HomeViewModel) {
    var editing by remember { mutableStateOf(false) }
    val cfg = vm.cloudConfig
    val info = vm.info
    SectionCard("云端连接（外网控制）") {
        InfoRow("服务器", cfg?.let { "${it.host}:${it.port}" } ?: "未配置")
        InfoRow(
            "手机 → 云端",
            when {
                cfg == null -> "—"
                vm.cloudConnected -> "已连接"
                vm.cloudError != null -> "失败：${vm.cloudError}"
                else -> "连接中…"
            },
            if (vm.cloudConnected) OkGreen else MaterialTheme.colorScheme.onSurfaceVariant,
        )
        InfoRow(
            "网关 → 云端",
            when {
                info == null -> "—"
                !info.cloudConfigured -> "未配置"
                info.cloudConnected -> "已连接"
                else -> "未连接"
            },
            if (info?.cloudConnected == true) OkGreen else MaterialTheme.colorScheme.onSurfaceVariant,
        )
        InfoRow(
            "当前链路",
            when (vm.transport) {
                Transport.Lan -> "局域网直连"
                Transport.Cloud -> "云端"
                Transport.None -> "—"
            },
        )
        OutlinedButton(onClick = { editing = true }, modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
            Text(if (cfg == null) "设置云端" else "修改云端设置")
        }
    }
    if (editing) {
        CloudDialog(
            initial = cfg,
            lanOk = vm.lanOk,
            onSave = {
                editing = false
                vm.saveCloud(it)
            },
            onDismiss = { editing = false },
        )
    }
}

@Composable
private fun CloudDialog(
    initial: CloudConfig?,
    lanOk: Boolean,
    onSave: (CloudConfig?) -> Unit,
    onDismiss: () -> Unit,
) {
    var host by remember { mutableStateOf(initial?.host ?: "") }
    var port by remember { mutableStateOf((initial?.port ?: 8883).toString()) }
    var user by remember { mutableStateOf(initial?.username ?: "") }
    var pass by remember { mutableStateOf(initial?.password ?: "") }
    val valid = host.isNotBlank() && port.toIntOrNull() in 1..65535 && user.isNotBlank()
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("云端设置") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "填 EMQX Cloud 部署的连接地址（MQTT over TLS）和客户端认证账号，网关和手机共用。" +
                        if (lanOk) "" else "\n手机现在连不上网关局域网，只会保存到手机，网关需要在局域网内再下发一次。",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedTextField(host, { host = it.trim() }, label = { Text("地址") }, singleLine = true)
                OutlinedTextField(
                    port, { port = it.filter(Char::isDigit) }, label = { Text("端口") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(user, { user = it.trim() }, label = { Text("用户名") }, singleLine = true)
                OutlinedTextField(
                    pass, { pass = it }, label = { Text("密码") }, singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                )
            }
        },
        confirmButton = {
            TextButton(enabled = valid, onClick = {
                onSave(CloudConfig(host, port.toInt(), user, pass))
            }) { Text(if (lanOk) "保存并下发到网关" else "保存") }
        },
        dismissButton = {
            Row {
                if (initial != null) {
                    TextButton(onClick = { onSave(null) }) { Text("关闭云端", color = MaterialTheme.colorScheme.error) }
                }
                TextButton(onClick = onDismiss) { Text("取消") }
            }
        },
    )
}
