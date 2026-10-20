package com.homenet.espgw.ui

import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.viewmodel.compose.viewModel

private enum class Screen { Setup, Provision, Bind, Home }

/** 根界面：没有网关时进入添加流程，有网关时进入主界面 */
@Composable
fun EspGwApp() {
    val home: HomeViewModel = viewModel()
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
        Screen.Home -> HomeScreen(vm = home, onAddGateway = { adding = Screen.Setup })
    }
}
