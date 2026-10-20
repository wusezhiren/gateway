package com.homenet.espgw.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.homenet.espgw.data.Gateway

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BindScreen(onBack: () -> Unit, onDone: (Gateway) -> Unit) {
    val vm: BindViewModel = viewModel()
    LaunchedEffect(Unit) { vm.startDiscovery() }
    LaunchedEffect(vm.result) { vm.result?.let(onDone) }

    Scaffold(topBar = {
        TopAppBar(
            title = { Text("绑定网关") },
            navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "返回") } },
        )
    }) { pad ->
        Column(
            Modifier.fillMaxSize().padding(pad).padding(16.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            vm.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            val waiting = vm.waiting
            if (waiting != null) {
                SectionCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp)
                        Text("  请在网关屏幕上点“允许”", style = MaterialTheme.typography.titleMedium)
                    }
                    Text("网关 $waiting，60 秒内有效", modifier = Modifier.padding(top = 8.dp))
                    TextButton(onClick = { vm.cancelBind() }) { Text("取消") }
                }
                return@Column
            }
            SectionCard("局域网里的网关") {
                if (vm.found.isEmpty()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                        Text("  正在查找…（手机需和网关连同一个 Wi-Fi）")
                    }
                }
                vm.found.forEachIndexed { i, gw ->
                    if (i > 0) HorizontalDivider()
                    ListItem(
                        headlineContent = { Text(gw.name) },
                        supportingContent = { Text("${gw.ip}  v${gw.version}") },
                        trailingContent = { Button(onClick = { vm.bind(gw.ip) }) { Text("绑定") } },
                    )
                }
            }
            SectionCard("手动输入地址") {
                Text("找不到时，在网关屏幕“概览”页查看 IP 地址", style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(
                    value = vm.manualIp,
                    onValueChange = { vm.manualIp = it },
                    label = { Text("网关 IP，如 192.168.1.20") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                )
                OutlinedButton(
                    onClick = { vm.bind(vm.manualIp) },
                    enabled = vm.manualIp.isNotBlank(),
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                ) { Text("绑定") }
            }
        }
    }
}
