package com.homenet.espgw.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.viewmodel.compose.viewModel

private enum class Screen { Setup, Provision, Bind, Home }

/** 根界面：没有网关时进入添加流程，有网关时进入主界面 */
@Composable
fun EspGwApp() {
    val home: HomeViewModel = viewModel()
    val upd: UpdateViewModel = viewModel()
    LaunchedEffect(Unit) { upd.check(manual = false) }
    // 云端通知有新版本时再查一次
    LaunchedEffect(home.appLatestCode) { if (home.appLatestCode != null) upd.check(manual = false) }
    LifecycleResumeEffect(Unit) {
        upd.onResume()
        onPauseOrDispose { }
    }
    if (upd.showDialog) UpdateDialog(upd)
    var adding by rememberSaveable { mutableStateOf<Screen?>(null) }
    val screen = adding ?: if (home.gateway == null) Screen.Setup else Screen.Home

    when (screen) {
        Screen.Setup -> SetupScreen(
            hasGateway = home.gateway != null,
            onProvision = { adding = Screen.Provision },
            onBind = { adding = Screen.Bind },
            onBack = { adding = null },
        )
        Screen.Provision -> {
            BackHandler { adding = Screen.Setup }
            ProvisionScreen(
                onBack = { adding = Screen.Setup },
                onDone = {
                    home.saveGateway(it)
                    adding = null
                },
            )
        }
        Screen.Bind -> {
            BackHandler { adding = Screen.Setup }
            BindScreen(
                onBack = { adding = Screen.Setup },
                onDone = {
                    home.saveGateway(it)
                    adding = null
                },
            )
        }
        Screen.Home -> HomeScreen(vm = home, upd = upd, onAddGateway = { adding = Screen.Setup })
    }
}

@Composable
private fun UpdateDialog(upd: UpdateViewModel) {
    val rel = upd.latest ?: return
    val p = upd.progress
    AlertDialog(
        onDismissRequest = { if (p == null) upd.dismiss() },
        title = { Text("发现新版本 v${rel.versionName}") },
        text = {
            Column {
                Text("当前 v${upd.currentVersion}，安装包 ${"%.1f".format(rel.size / 1048576.0)} MB")
                if (rel.gatewayVersion.isNotBlank()) Text("内置网关固件 v${rel.gatewayVersion}")
                if (rel.notes.isNotBlank()) Text(rel.notes, modifier = Modifier.padding(top = 8.dp))
                if (p != null) {
                    Text("下载中 ${(p * 100).toInt()}%", modifier = Modifier.padding(top = 12.dp))
                    LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth().padding(top = 4.dp))
                }
                upd.error?.let {
                    Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(top = 8.dp))
                }
            }
        },
        confirmButton = { TextButton(onClick = { upd.install() }, enabled = p == null) { Text("更新") } },
        dismissButton = { TextButton(onClick = { upd.dismiss() }, enabled = p == null) { Text("以后再说") } },
    )
}
