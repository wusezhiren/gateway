package com.homenet.espgw.ui

import android.app.Application
import android.os.Build
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.homenet.espgw.data.Discovery
import com.homenet.espgw.data.FoundGateway
import com.homenet.espgw.data.Gateway
import com.homenet.espgw.data.GwApi
import com.homenet.espgw.data.userMessage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.launch

/** 绑定已联网的网关：mDNS 发现或手动输入 IP → 网关屏幕上点“允许” → 拿到 token */
class BindViewModel(app: Application) : AndroidViewModel(app) {
    var found by mutableStateOf<List<FoundGateway>>(emptyList())
        private set
    var manualIp by mutableStateOf("")
    /** 正在等屏幕确认的网关地址 */
    var waiting by mutableStateOf<String?>(null)
        private set
    var error by mutableStateOf<String?>(null)
    var result by mutableStateOf<Gateway?>(null)
        private set

    private var discoverJob: Job? = null
    private var bindJob: Job? = null

    fun startDiscovery() {
        if (discoverJob?.isActive == true) return
        discoverJob = viewModelScope.launch {
            Discovery(getApplication()).discover()
                .catch { error = it.message }
                .collect { found = it }
        }
    }

    fun bind(ip: String) {
        val host = ip.trim()
        if (host.isEmpty()) return
        error = null
        bindJob?.cancel()
        bindJob = viewModelScope.launch {
            try {
                val api = GwApi(host)
                val info = api.info()
                if (info.project != "esp_s3_gateway") {
                    error = "$host 不是网关"
                    return@launch
                }
                val id = api.bindStart(phoneName())
                waiting = host
                val deadline = System.currentTimeMillis() + 70_000
                while (System.currentTimeMillis() < deadline) {
                    delay(1500)
                    val r = try {
                        api.bindPoll(id)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        continue
                    }
                    when (r.state) {
                        "allowed" -> {
                            val suffix = info.mac.replace(":", "").takeLast(4).uppercase()
                            result = Gateway(info.mac, host, r.token, "ESP-GW-$suffix")
                            return@launch
                        }
                        "pending" -> Unit
                        else -> {
                            error = "网关上已拒绝或超时"
                            return@launch
                        }
                    }
                }
                error = "等待超时"
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.userMessage()
            } finally {
                waiting = null
            }
        }
    }

    fun cancelBind() {
        bindJob?.cancel()
        waiting = null
    }

    /** 网关屏幕上显示的手机名：只用 ASCII，避免缺字 */
    private fun phoneName(): String =
        "${Build.MANUFACTURER} ${Build.MODEL}".filter { it.code in 0x20..0x7e }.trim().take(24).ifBlank { "Android" }
}
