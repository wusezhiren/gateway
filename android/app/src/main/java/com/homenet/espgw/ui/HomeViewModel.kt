package com.homenet.espgw.ui

import android.app.Application
import android.net.ConnectivityManager
import android.net.Network
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.homenet.espgw.data.ApiException
import com.homenet.espgw.data.CloudConfig
import com.homenet.espgw.data.CloudLink
import com.homenet.espgw.data.Discovery
import com.homenet.espgw.data.Gateway
import com.homenet.espgw.data.GatewayStore
import com.homenet.espgw.data.GwApi
import com.homenet.espgw.data.GwInfo
import com.homenet.espgw.data.Node
import com.homenet.espgw.data.userMessage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject

sealed interface OtaUi {
    data object Idle : OtaUi
    data class Uploading(val sent: Long, val total: Long) : OtaUi
    data class Rebooting(val version: String) : OtaUi
    data class Done(val version: String) : OtaUi
    data class Failed(val msg: String) : OtaUi
    /** 版本不高于当前，等用户确认是否强制 */
    data class NeedForce(val uri: Uri) : OtaUi
}

/** 当前通过哪条链路和网关通信 */
enum class Transport { None, Lan, Cloud }

/**
 * 已绑定网关的主界面。
 * 配了云端时优先走云端（网关状态由 MQTT 推送），否则或云端不通时走局域网 HTTP 轮询。
 * 网关固件升级只走局域网。
 */
class HomeViewModel(app: Application) : AndroidViewModel(app) {
    private val store = GatewayStore(app)

    var gateway by mutableStateOf(store.load())
        private set
    var cloudConfig by mutableStateOf(store.loadCloud())
        private set
    var info by mutableStateOf<GwInfo?>(null)
        private set
    var nodes by mutableStateOf<List<Node>>(emptyList())
        private set
    var pairLeft by mutableIntStateOf(0)
        private set
    var transport by mutableStateOf(Transport.None)
        private set
    /** 局域网能直连网关（固件升级、下发云端配置需要） */
    var lanOk by mutableStateOf(false)
        private set
    /** 手机到云端的连接状态与错误 */
    var cloudConnected by mutableStateOf(false)
        private set
    var cloudError by mutableStateOf<String?>(null)
        private set
    /** 连不上网关时的提示，连上后清空 */
    var linkError by mutableStateOf<String?>(null)
        private set
    /** 一次性提示（snackbar），显示后由界面清掉 */
    var message by mutableStateOf<String?>(null)
    var ota by mutableStateOf<OtaUi>(OtaUi.Idle)

    private var pollJob: Job? = null
    private var cloud: CloudLink? = null
    private var cloudJobs = mutableListOf<Job>()
    private var lanFailures = 0
    private var lastLanTry = 0L

    private val cm = app.getSystemService(ConnectivityManager::class.java)
    private var netCallback: ConnectivityManager.NetworkCallback? = null
    private var defaultNet: Network? = null

    private fun api(): GwApi? = gateway?.let { GwApi(it.ip, it.token) }

    /** 云端可用：手机连上云端且网关在线 */
    private fun cloudUsable(): Boolean = cloud?.let { it.connected.value && it.gwOnline.value == true } == true

    fun saveGateway(gw: Gateway?) {
        if (gw == null) store.clear() else store.save(gw)
        val changed = gw?.mac != gateway?.mac || gw?.token != gateway?.token
        gateway = gw
        if (changed) {
            info = null
            nodes = emptyList()
            linkError = null
            lanFailures = 0
            restartCloud()
        }
    }

    fun startPolling() {
        if (pollJob?.isActive == true) return
        watchNetwork()
        startCloud()
        pollJob = viewModelScope.launch {
            while (isActive) {
                tick()
                delay(1000)
            }
        }
    }

    fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
        netCallback?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        netCallback = null
        defaultNet = null
        stopCloud()
    }

    /**
     * 手机默认网络变化（Wi-Fi ⇄ 4G）时，旧的云端连接绑在已断开的网络上，
     * 等它自己超时重连要很久：直接重建，并立刻重新判断走局域网还是云端。
     */
    private fun watchNetwork() {
        if (netCallback != null) return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                val changed = defaultNet != null && defaultNet != network
                defaultNet = network
                if (changed) viewModelScope.launch { onNetworkChanged() }
            }
        }
        netCallback = cb
        runCatching { cm.registerDefaultNetworkCallback(cb) }
    }

    private fun onNetworkChanged() {
        transport = Transport.None
        lanOk = false
        lanFailures = 0
        lastLanTry = 0
        restartCloud()
    }

    // ---------- 云端 ----------

    private fun startCloud() {
        val gw = gateway ?: return
        val cfg = cloudConfig ?: return
        if (cloud != null) return
        val link = CloudLink(cfg, gw.cloudId, gw.token, viewModelScope)
        cloud = link
        link.start()
        cloudJobs += viewModelScope.launch { link.connected.collect { cloudConnected = it } }
        cloudJobs += viewModelScope.launch { link.error.collect { cloudError = it } }
        cloudJobs += viewModelScope.launch {
            link.state.collect { st ->
                st ?: return@collect
                // 局域网直连时以 HTTP 轮询为准，避免两边数据来回跳
                if (transport != Transport.Lan) {
                    info = st.info
                    nodes = st.nodes.nodes
                    val elapsed = ((System.currentTimeMillis() - st.receivedAt) / 1000).toInt()
                    pairLeft = (st.nodes.pairLeft - elapsed).coerceAtLeast(0)
                }
            }
        }
    }

    private fun stopCloud() {
        cloudJobs.forEach { it.cancel() }
        cloudJobs.clear()
        cloud?.close()
        cloud = null
        cloudConnected = false
    }

    private fun restartCloud() {
        val running = pollJob?.isActive == true
        stopCloud()
        if (running) startCloud()
    }

    fun saveCloud(cfg: CloudConfig?) {
        store.saveCloud(cfg)
        cloudConfig = cfg
        restartCloud()
        // 同时下发给网关（需要局域网可达）
        val api = api() ?: return
        viewModelScope.launch {
            message = try {
                api.setCloud(cfg ?: CloudConfig("", 8883, "", ""))
                if (cfg == null) "已关闭云端连接" else "已下发到网关，稍等网关连上云端"
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                "已保存到手机，但没能下发到网关（需在局域网内）：${e.userMessage()}"
            }
        }
    }

    // ---------- 轮询 ----------

    /**
     * 每秒一次：局域网能通就 2 秒轮询一次 HTTP（最快、数据最准）；
     * 不通但云端可用就用云端推送，并每 15 秒探一下局域网；都不通就继续重试局域网。
     */
    private suspend fun tick() {
        val now = System.currentTimeMillis()
        if (transport != Transport.Lan && cloudUsable()) {
            transport = Transport.Cloud
            linkError = null
            if (pairLeft > 0) pairLeft--
            if (now - lastLanTry >= 15_000) {
                lastLanTry = now
                refreshLan(quiet = true)
            }
        } else if (now - lastLanTry >= 2000) {
            lastLanTry = now
            refreshLan(quiet = false)
        }
    }

    suspend fun refresh() = refreshLan(quiet = false)

    private suspend fun refreshLan(quiet: Boolean) {
        val api = api() ?: return
        try {
            val i = api.info()
            val nl = api.nodes()
            info = i
            nodes = nl.nodes
            pairLeft = nl.pairLeft
            linkError = null
            lanFailures = 0
            lanOk = true
            transport = Transport.Lan
            (ota as? OtaUi.Rebooting)?.let {
                if (i.version == it.version) ota = OtaUi.Done(it.version)
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            lanOk = false
            lanFailures++
            if (cloudUsable()) {
                transport = Transport.Cloud
                return
            }
            transport = Transport.None
            if (!quiet) {
                val c = cloud
                linkError = when {
                    c == null -> e.userMessage()
                    c.connected.value && c.gwOnline.value == false -> "网关不在线"
                    !c.connected.value -> "正在通过云端连接…" + (c.error.value?.let { "（$it）" } ?: "")
                    else -> "正在等待网关状态…"
                }
            }
            // 连续失败时用 mDNS 按 MAC 找一下，网关 IP 可能变了
            if (lanFailures == 3 && e !is ApiException) rediscover()
        }
    }

    private suspend fun rediscover() {
        val gw = gateway ?: return
        val found = Discovery(getApplication()).findByMac(gw.mac) ?: return
        if (found.ip != gw.ip) {
            saveGateway(gw.copy(ip = found.ip))
            message = "网关地址已更新为 ${found.ip}"
        }
    }

    // ---------- 命令 ----------

    private fun action(op: String, args: JSONObject, http: suspend (GwApi) -> Unit, done: String? = null) {
        viewModelScope.launch {
            try {
                val c = cloud
                if (transport == Transport.Lan || c == null || !cloudUsable()) {
                    http(api() ?: return@launch)
                    refreshLan(quiet = true)
                } else {
                    c.command(op, args)     // 网关执行后会推送新状态
                }
                done?.let { message = it }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                message = e.userMessage()
            }
        }
    }

    fun setChannel(node: Node, ch: Int, value: Int) =
        action("set", JSONObject().put("mac", node.mac).put("ch", ch).put("value", value),
            { it.set(node.mac, ch, value) })

    fun unpair(node: Node) =
        action("unpair", JSONObject().put("mac", node.mac), { it.unpair(node.mac) }, "已解绑 ${node.title}")

    fun pair(seconds: Int) {
        pairLeft = seconds
        action("pair", JSONObject().put("seconds", seconds), { it.pair(seconds) })
    }

    // ---------- 网关升级（仅局域网） ----------

    fun uploadFirmware(uri: Uri, force: Boolean = false) {
        val api = api() ?: return
        val cr = getApplication<Application>().contentResolver
        viewModelScope.launch {
            try {
                val size = cr.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
                    if (c.moveToFirst()) c.getLong(0) else -1L
                } ?: -1L
                if (size <= 0) throw IllegalArgumentException("读不到文件大小")
                ota = OtaUi.Uploading(0, size)
                val version = cr.openInputStream(uri)?.use { input ->
                    api.otaGateway(input, size, force) { sent -> ota = OtaUi.Uploading(sent, size) }
                } ?: throw IllegalArgumentException("打不开文件")
                ota = OtaUi.Rebooting(version)
                // 60 秒内没看到新版本算失败（可能被回滚）
                delay(60_000)
                if (ota is OtaUi.Rebooting) ota = OtaUi.Failed("网关没有以 v$version 启动，可能已回滚，请查看网关屏幕")
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                ota = if (e.error == "not_newer") OtaUi.NeedForce(uri) else OtaUi.Failed(e.userMessage())
            } catch (e: Exception) {
                ota = OtaUi.Failed(e.userMessage())
            }
        }
    }

    override fun onCleared() {
        stopPolling()
    }
}
