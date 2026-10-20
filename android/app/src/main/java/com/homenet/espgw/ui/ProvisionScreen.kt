package com.homenet.espgw.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.homenet.espgw.data.Gateway
import com.homenet.espgw.data.Hotspot
import com.homenet.espgw.ui.theme.OkGreen

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProvisionScreen(onBack: () -> Unit, onDone: (Gateway) -> Unit) {
    val vm: ProvisionViewModel = viewModel()
    val ctx = LocalContext.current

    // 读手机当前 Wi-Fi 名称需要定位权限
    val perm = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { vm.readPhoneSsid() }
    LaunchedEffect(Unit) {
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            vm.readPhoneSsid()
        } else {
            perm.launch(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    Scaffold(topBar = {
        TopAppBar(
            title = { Text("网关配网") },
            navigationIcon = {
                IconButton(onClick = {
                    vm.cancel()
                    onBack()
                }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "返回") }
            },
        )
    }) { pad ->
        Column(
            Modifier.fillMaxSize().padding(pad).padding(16.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            vm.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            when (vm.step) {
                ProvisionViewModel.Step.Intro -> IntroStep(vm)
                ProvisionViewModel.Step.Joining -> Busy("正在连接网关热点…\n在系统弹窗中选择 ${Hotspot.SSID_PREFIX}XXXX")
                ProvisionViewModel.Step.Pick -> PickStep(vm)
                ProvisionViewModel.Step.Connecting -> Busy(vm.progress)
                ProvisionViewModel.Step.Done -> {
                    val gw = vm.result!!
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.CheckCircle, null, tint = OkGreen, modifier = Modifier.size(32.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("配网成功", style = MaterialTheme.typography.headlineSmall)
                    }
                    Text("${gw.name} 已连上 ${vm.ssid}，地址 ${gw.ip}")
                    Button(onClick = { onDone(gw) }, modifier = Modifier.fillMaxWidth()) { Text("完成") }
                }
            }
        }
    }
}

@Composable
private fun IntroStep(vm: ProvisionViewModel) {
    SectionCard("1. 网关进入配网模式") {
        Text("未联网的网关开机会自动进入配网；已联网的网关长按 REC 键 3 秒，并在屏幕上确认。屏幕出现二维码即可。")
    }
    SectionCard("2. 连接网关热点") {
        Text("App 会请求连接 ${Hotspot.SSID_PREFIX}XXXX 热点（密码 ${Hotspot.PASSWORD}），请在系统弹窗中选择它。")
        vm.phoneSsid?.let {
            Text("手机当前 Wi-Fi：$it", color = MaterialTheme.colorScheme.primary, modifier = Modifier.padding(top = 8.dp))
        }
        Button(onClick = { vm.joinHotspot() }, modifier = Modifier.fillMaxWidth().padding(top = 12.dp)) {
            Text("连接网关热点")
        }
    }
}

@Composable
private fun PickStep(vm: ProvisionViewModel) {
    var showPass by remember { mutableStateOf(false) }
    SectionCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("网关附近的 Wi-Fi（仅 2.4GHz）", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            if (vm.scanning) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
            } else {
                IconButton(onClick = { vm.rescan() }) { Icon(Icons.Default.Refresh, "重新扫描") }
            }
        }
        vm.scan.forEachIndexed { i, ap ->
            if (i > 0) HorizontalDivider()
            Row(
                Modifier.fillMaxWidth().clickable { vm.ssid = ap.ssid }.padding(vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = vm.ssid == ap.ssid, onClick = { vm.ssid = ap.ssid })
                Text(ap.ssid, modifier = Modifier.weight(1f))
                if (ap.ssid == vm.phoneSsid) {
                    Text("手机当前", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.labelSmall)
                    Spacer(Modifier.width(8.dp))
                }
                if (ap.secure) Icon(Icons.Default.Lock, "加密", Modifier.size(16.dp))
                Text(" ${ap.rssi}", style = MaterialTheme.typography.labelSmall)
            }
        }
        vm.phoneSsid?.let { p ->
            if (!vm.scanning && vm.scan.isNotEmpty() && vm.scan.none { it.ssid == p }) {
                Text("网关没扫到手机当前的“$p”，它可能是 5GHz 网络", color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall)
            }
        }
    }
    OutlinedTextField(
        value = vm.ssid,
        onValueChange = { vm.ssid = it },
        label = { Text("Wi-Fi 名称") },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedTextField(
        value = vm.password,
        onValueChange = { vm.password = it },
        label = { Text("Wi-Fi 密码") },
        singleLine = true,
        visualTransformation = if (showPass) VisualTransformation.None else PasswordVisualTransformation(),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
        trailingIcon = { TextButton(onClick = { showPass = !showPass }) { Text(if (showPass) "隐藏" else "显示") } },
        modifier = Modifier.fillMaxWidth(),
    )
    Button(onClick = { vm.connect() }, modifier = Modifier.fillMaxWidth()) { Text("开始配网") }
    OutlinedButton(onClick = { vm.cancel() }, modifier = Modifier.fillMaxWidth()) { Text("断开网关热点") }
}

@Composable
private fun Busy(text: String) {
    Column(
        Modifier.fillMaxWidth().padding(top = 48.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        CircularProgressIndicator()
        Text(text, style = MaterialTheme.typography.bodyLarge)
    }
}
