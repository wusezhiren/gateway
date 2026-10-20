package com.homenet.espgw.ui

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.homenet.espgw.data.Gateway
import com.homenet.espgw.data.GwApi
import com.homenet.espgw.data.GwInfo
import com.homenet.espgw.data.Hotspot
import com.homenet.espgw.data.ScanAp
import com.homenet.espgw.data.userMessage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * 配网：连网关热点 → 网关扫描附近 Wi-Fi → 选网络填密码 → 下发 → 轮询结果。
 * 协议见 docs/api.md「配网接口」。
 */
class ProvisionViewModel(app: Application) : AndroidViewModel(app) {
    enum class Step { Intro, Joining, Pick, Connecting, Done }

    private val hotspot = Hotspot(app)

    var step by mutableStateOf(Step.Intro)
        private set
    var phoneSsid by mutableStateOf<String?>(null)
        private set
    var scan by mutableStateOf<List<ScanAp>>(emptyList())
        private set
    var scanning by mutableStateOf(false)
        private set
    var ssid by mutableStateOf("")
    var password by mutableStateOf("")
    var progress by mutableStateOf("")
        private set
    var error by mutableStateOf<String?>(null)
    var result by mutableStateOf<Gateway?>(null)
        private set

    private var info: GwInfo? = null
    private var job: Job? = null

    fun readPhoneSsid() {
        phoneSsid = Hotspot.currentSsid(getApplication())
        if (ssid.isBlank()) phoneSsid?.let { ssid = it }
    }

    private suspend fun api(): GwApi {
        val net = withTimeoutOrNull(20_000) { hotspot.network.filterNotNull().first() }
            ?: throw IllegalStateException("和网关热点的连接断开了")
        return GwApi(GW_AP_IP, network = net)
    }

    fun joinHotspot() {
        error = null
        step = Step.Joining
        hotspot.request()
        job?.cancel()
        job = viewModelScope.launch {
            // 等系统选择框连上，或用户取消
            val ok = withTimeoutOrNull(120_000) {
                while (hotspot.network.value == null && !hotspot.unavailable.value) delay(200)
                hotspot.network.value != null
            } ?: false
            if (!ok) {
                hotspot.release()
                error = "没有连上网关热点。请确认网关屏幕正在显示配网二维码"
                step = Step.Intro
                return@launch
            }
            try {
                info = api().info()
                step = Step.Pick
                rescan()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.userMessage()
                step = Step.Intro
                hotspot.release()
            }
        }
    }

    fun rescan() {
        viewModelScope.launch {
            scanning = true
            try {
                scan = api().provScan()
                if (ssid.isBlank()) scan.firstOrNull()?.let { ssid = it.ssid }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = "扫描失败：${e.userMessage()}"
            } finally {
                scanning = false
            }
        }
    }

    fun connect() {
        error = null
        val s = ssid.trim()
        if (s.isEmpty()) {
            error = "请选择或输入 Wi-Fi 名称"
            return
        }
        if (password.isNotEmpty() && password.length < 8) {
            error = "Wi-Fi 密码至少 8 位"
            return
        }
        step = Step.Connecting
        progress = "正在发送…"
        job?.cancel()
        job = viewModelScope.launch {
            try {
                api().provConnect(s, password)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.userMessage()
                step = Step.Pick
                return@launch
            }
            progress = "网关正在连接 $s…"
            // 网关连上路由器后热点会切到路由器信道，期间请求可能失败，继续重试
            val deadline = System.currentTimeMillis() + 90_000
            while (System.currentTimeMillis() < deadline) {
                delay(1000)
                val st = try {
                    api().provStatus()
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    progress = "网关正在切换信道，请稍候…"
                    continue
                }
                when (st.state) {
                    "done" -> {
                        finish(st.ip, st.token)
                        return@launch
                    }
                    "failed" -> {
                        error = when (st.reason) {
                            "bad_password" -> "密码错误"
                            "no_ap" -> "网关找不到 ${st.ssid}，请确认是 2.4GHz 网络"
                            "no_ip" -> "连上了但没有获取到 IP，请检查路由器"
                            else -> "连接失败，请重试"
                        }
                        step = Step.Pick
                        return@launch
                    }
                    "connecting" -> progress = "网关正在连接 $s…"
                }
            }
            error = "没有收到结果。如果网关屏幕显示配网成功，请返回用“绑定已联网的网关”添加"
            step = Step.Pick
        }
    }

    private fun finish(ip: String, token: String) {
        val mac = info?.mac ?: ""
        val suffix = mac.replace(":", "").takeLast(4).uppercase()
        result = Gateway(mac = mac, ip = ip, token = token, name = "ESP-GW-$suffix")
        hotspot.release()
        step = Step.Done
    }

    fun cancel() {
        job?.cancel()
        hotspot.release()
        step = Step.Intro
    }

    override fun onCleared() {
        hotspot.release()
    }

    companion object {
        const val GW_AP_IP = "192.168.4.1"
    }
}
