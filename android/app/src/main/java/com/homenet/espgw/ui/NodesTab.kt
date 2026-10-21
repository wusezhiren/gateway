package com.homenet.espgw.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.homenet.espgw.data.Channel
import com.homenet.espgw.data.Node
import com.homenet.espgw.data.NodeFirmware
import com.homenet.espgw.data.NodeOtaStatus
import com.homenet.espgw.data.gatewayErrorText
import com.homenet.espgw.ui.theme.OkGreen
import com.homenet.espgw.ui.theme.WarnOrange

@Composable
fun NodesTab(vm: HomeViewModel) {
    var detail by remember { mutableStateOf<String?>(null) }   // 详情弹窗节点 MAC

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item { PairCard(vm) }
        if (vm.nodes.isEmpty() && vm.info != null) {
            item {
                Text(
                    "还没有节点。点上面的“配对新节点”，然后给节点上电。",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(8.dp),
                )
            }
        }
        val nodeOta = vm.info?.nodeOta
        items(vm.nodes, key = { it.mac }) { node ->
            NodeCard(
                node,
                upgrade = vm.nodeUpgrade(node),
                ota = nodeOta?.takeIf { it.mac.equals(node.mac, ignoreCase = true) && it.state != "idle" },
                otaBusy = nodeOta?.active == true,
                onUpgrade = { vm.upgradeNode(node) },
                onOpen = { detail = node.mac },
                onSet = { ch, v -> vm.setChannel(node, ch, v) },
            )
        }
    }

    vm.nodes.firstOrNull { it.mac == detail }?.let { node ->
        NodeDetailDialog(node, onDismiss = { detail = null }, onUnpair = {
            detail = null
            vm.unpair(node)
        })
    }
}

@Composable
private fun PairCard(vm: HomeViewModel) {
    val left = vm.pairLeft
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(if (left > 0) "配对中，剩余 $left 秒" else "添加节点", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (left > 0) "给新节点上电，或长按节点上的复位键" else "打开 60 秒配对窗口，新节点上电后自动加入",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (left > 0) {
                Button(
                    onClick = { vm.pair(0) },
                    colors = ButtonDefaults.buttonColors(containerColor = WarnOrange),
                ) { Text("停止") }
            } else {
                FilledTonalButton(onClick = { vm.pair(60) }, enabled = vm.info != null) {
                    Icon(Icons.Default.Add, null, Modifier.size(18.dp))
                    Text(" 配对新节点")
                }
            }
        }
    }
}

@Composable
private fun StatusDot(online: Boolean) {
    Box(
        Modifier.size(10.dp).clip(CircleShape)
            .background(if (online) OkGreen else Color.Gray)
    )
}

@Composable
private fun NodeCard(
    node: Node,
    upgrade: NodeFirmware?,
    ota: NodeOtaStatus?,
    otaBusy: Boolean,
    onUpgrade: () -> Unit,
    onOpen: () -> Unit,
    onSet: (Int, Int) -> Unit,
) {
    var confirm by remember { mutableStateOf(false) }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(Modifier.fillMaxWidth().clickable(onClick = onOpen), verticalAlignment = Alignment.CenterVertically) {
                StatusDot(node.online)
                Spacer(Modifier.width(8.dp))
                Column(Modifier.weight(1f)) {
                    Text(node.title, style = MaterialTheme.typography.titleMedium)
                    Text(
                        if (node.online) "${node.chip} · v${node.version} · 信号 ${rssiText(node.rssi)}"
                        else "离线" + if (node.lastSeenS >= 0) " · ${formatUptime(node.lastSeenS)}前在线" else "",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                TextButton(onClick = onOpen) { Text("详情") }
            }
            if (ota != null && (ota.state != "done" || upgrade == null)) {
                NodeOtaLine(ota)
            } else if (upgrade != null) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "有新固件 v${upgrade.version}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = { confirm = true }, enabled = node.online && !otaBusy) { Text("升级") }
                }
            }
            node.ch.forEachIndexed { i, ch ->
                ChannelControl(i, ch, node.online) { onSet(i, it) }
            }
        }
    }
    if (confirm && upgrade != null) {
        ConfirmDialog(
            title = "升级节点",
            text = "把 ${node.title} 从 v${node.version} 升级到 v${upgrade.version}。" +
                "网关下载固件后传给节点，约 1 分钟，期间开关照常可用，最后节点会重启几秒。" +
                "新固件连不上网关会自动回滚到旧版本。",
            confirm = "升级",
            onConfirm = {
                confirm = false
                onUpgrade()
            },
            onDismiss = { confirm = false },
        )
    }
}

@Composable
private fun NodeOtaLine(ota: NodeOtaStatus) {
    val pct = if (ota.total > 0) " ${ota.received * 100 / ota.total}%" else ""
    val (text, color) = when (ota.state) {
        "downloading" -> "网关下载固件$pct" to MaterialTheme.colorScheme.primary
        "sending" -> "正在传给节点$pct" to MaterialTheme.colorScheme.primary
        "rebooting" -> "节点校验通过，重启中…" to MaterialTheme.colorScheme.primary
        "done" -> "已升级到 v${ota.version}" to OkGreen
        "failed" -> "升级失败：${gatewayErrorText(ota.error)}" to MaterialTheme.colorScheme.error
        else -> return
    }
    Text(text, style = MaterialTheme.typography.bodySmall, color = color, modifier = Modifier.padding(top = 4.dp))
}

@Composable
private fun ChannelControl(index: Int, ch: Channel, enabled: Boolean, onSet: (Int) -> Unit) {
    val pending = ch.desired != null && ch.desired != ch.value
    Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(
            when (ch.type) {
                "relay" -> "开关 ${index + 1}"
                "pwm" -> "调光 ${index + 1}"
                else -> "通道 ${index + 1}"
            } + if (pending) "（同步中）" else "",
            modifier = Modifier.width(110.dp),
        )
        when (ch.type) {
            "relay" -> {
                Spacer(Modifier.weight(1f))
                Switch(
                    checked = (ch.desired ?: ch.value) != 0,
                    onCheckedChange = { onSet(if (it) 1 else 0) },
                    enabled = enabled,
                )
            }
            "pwm" -> {
                // 拖动时只改本地值，松手才发送
                var dragging by remember { mutableStateOf(false) }
                var local by remember { mutableFloatStateOf(ch.value.toFloat()) }
                val shown = if (dragging) local else (ch.desired ?: ch.value).toFloat()
                Slider(
                    value = shown,
                    onValueChange = {
                        dragging = true
                        local = it
                    },
                    onValueChangeFinished = {
                        dragging = false
                        onSet(local.toInt())
                    },
                    valueRange = 0f..ch.max.toFloat().coerceAtLeast(1f),
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    "${(shown * 100 / ch.max.coerceAtLeast(1)).toInt()}%",
                    modifier = Modifier.width(48.dp).padding(start = 8.dp),
                )
            }
            else -> Text("值 ${ch.value}")
        }
    }
}

@Composable
private fun NodeDetailDialog(node: Node, onDismiss: () -> Unit, onUnpair: () -> Unit) {
    var confirm by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(node.title) },
        text = {
            Column {
                InfoRow("状态", if (node.online) "在线" else "离线", if (node.online) OkGreen else Color.Gray)
                InfoRow("MAC", node.mac)
                InfoRow("芯片", node.chip)
                InfoRow("固件", "${node.project} v${node.version}")
                InfoRow("信号", rssiText(node.rssi))
                InfoRow("运行时间", formatUptime(node.uptimeS))
                InfoRow("通道数", node.ch.size.toString())
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("关闭") } },
        dismissButton = {
            TextButton(onClick = { confirm = true }) {
                Text("解绑", color = MaterialTheme.colorScheme.error)
            }
        },
    )
    if (confirm) {
        ConfirmDialog(
            title = "解绑节点",
            text = "解绑后 ${node.title} 需要重新配对才能使用。",
            confirm = "解绑",
            onConfirm = onUnpair,
            onDismiss = { confirm = false },
        )
    }
}
