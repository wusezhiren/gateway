package com.homenet.espgw.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.homenet.espgw.ui.theme.OkGreen

@Composable
fun FirmwareTab(vm: HomeViewModel) {
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { vm.uploadFirmware(it) }
    }
    val info = vm.info
    val ota = vm.ota
    val busy = ota is OtaUi.Uploading || ota is OtaUi.Downloading || ota is OtaUi.Rebooting
    val remote = !vm.lanOk && vm.transport == Transport.Cloud

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        SectionCard("网关固件") {
            InfoRow("当前版本", info?.let { "v${it.version}" } ?: "—")
            InfoRow("运行分区", info?.partition ?: "—")
            InfoRow(
                "App 内置",
                vm.bundledGateway?.let { "v${it.version}" + if (vm.gatewayUpgradable) "（可升级）" else "" } ?: "无",
                if (vm.gatewayUpgradable) OkGreen else androidx.compose.ui.graphics.Color.Unspecified,
            )
            vm.latestRelease?.takeIf { it.gatewayUrl.isNotBlank() }?.let { rel ->
                InfoRow(
                    "最新发布",
                    "v${rel.gatewayVersion}" + if (vm.remoteUpgradable) "（可远程升级）" else "",
                    if (vm.remoteUpgradable) OkGreen else androidx.compose.ui.graphics.Color.Unspecified,
                )
            }
            Text(
                if (remote) {
                    "不在局域网时，网关会自己从发布仓库下载固件，校验签名和 SHA-256 后重启；" +
                        "新固件连不上 Wi-Fi 或云端会自动回滚到当前版本。"
                } else {
                    "选择签名后的网关固件（esp_s3_gateway.bin）。网关会校验签名，升级后自动重启，失败会自动回滚。"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            if (!vm.lanOk && !remote && !busy) {
                Text("固件升级需要手机和网关在同一局域网，或网关连上云端", color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 8.dp))
            }
            when (ota) {
                is OtaUi.Uploading -> {
                    val f = if (ota.total > 0) ota.sent.toFloat() / ota.total else 0f
                    Text("上传中 ${(f * 100).toInt()}%  请勿关闭 App", modifier = Modifier.padding(top = 12.dp))
                    LinearProgressIndicator(progress = { f }, modifier = Modifier.fillMaxWidth().padding(top = 4.dp))
                }
                is OtaUi.Downloading -> {
                    val f = if (ota.total > 0) ota.received.toFloat() / ota.total else 0f
                    Text(
                        if (ota.total > 0) "网关下载中 ${(f * 100).toInt()}%" else "等待网关开始下载…",
                        modifier = Modifier.padding(top = 12.dp),
                    )
                    if (ota.total > 0) {
                        LinearProgressIndicator(progress = { f }, modifier = Modifier.fillMaxWidth().padding(top = 4.dp))
                    } else {
                        LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 4.dp))
                    }
                }
                is OtaUi.Rebooting -> {
                    Text("校验通过，网关正在重启到 v${ota.version}…", modifier = Modifier.padding(top = 12.dp))
                    LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 4.dp))
                }
                is OtaUi.Done -> Text("升级成功，网关已运行 v${ota.version}", color = OkGreen,
                    modifier = Modifier.padding(top = 12.dp))
                is OtaUi.Failed -> Text("升级失败：${ota.msg}", color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 12.dp))
                is OtaUi.NeedForce -> {
                    Text("所选固件版本不高于网关当前版本", color = MaterialTheme.colorScheme.error,
                        modifier = Modifier.padding(top = 12.dp))
                    TextButton(onClick = { ota.retry() }) { Text("仍然安装（降级）") }
                }
                OtaUi.Idle -> Unit
            }
            vm.bundledGateway?.takeIf { vm.gatewayUpgradable }?.let { fw ->
                Button(
                    onClick = {
                        vm.ota = OtaUi.Idle
                        vm.upgradeBundled()
                    },
                    enabled = !busy && vm.lanOk,
                    modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                ) { Text("升级到 v${fw.version}") }
            }
            vm.latestRelease?.takeIf { remote && vm.remoteUpgradable }?.let { rel ->
                Button(
                    onClick = {
                        vm.ota = OtaUi.Idle
                        vm.upgradeRemote()
                    },
                    enabled = !busy,
                    modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                ) { Text("远程升级到 v${rel.gatewayVersion}") }
            }
            OutlinedButton(
                onClick = {
                    vm.ota = OtaUi.Idle
                    pick.launch(arrayOf("application/octet-stream", "*/*"))
                },
                enabled = !busy && vm.lanOk,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
            ) { Text("选择其他固件文件") }
        }
        SectionCard("节点固件") {
            Text(
                "节点无线升级（ESP-NOW 分片传输）将在网关支持后开放。",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
