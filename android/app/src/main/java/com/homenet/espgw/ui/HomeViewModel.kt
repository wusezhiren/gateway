package com.homenet.espgw.ui

import android.app.Application
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.homenet.espgw.data.ApiException
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

sealed interface OtaUi {
    data object Idle : OtaUi
    data class Uploading(val sent: Long, val total: Long) : OtaUi
    data class Rebooting(val version: String) : OtaUi
    data class Done(val version: String) : OtaUi
    data class Failed(val msg: String) : OtaUi
    /** 版本不高于当前，等用户确认是否强制 */
    data class NeedForce(val uri: Uri) : OtaUi
}

/** 已绑定网关的主界面：定时刷新、节点控制、配对、网关升级 */
class HomeViewModel(app: Application) : AndroidViewModel(app) {
    private val store = GatewayStore(app)

    var gateway by mutableStateOf(store.load())
        private set
    var info by mutableStateOf<GwInfo?>(null)
        private set
    var nodes by mutableStateOf<List<Node>>(emptyList())
        private set
    var pairLeft by mutableIntStateOf(0)
        private set
    /** 连不上网关时的提示，连上后清空 */
    var linkError by mutableStateOf<String?>(null)
        private set
    /** 一次性提示（snackbar），显示后由界面清掉 */
    var message by mutableStateOf<String?>(null)
    var ota by mutableStateOf<OtaUi>(OtaUi.Idle)

    private var pollJob: Job? = null
    private var failures = 0

    private fun api(): GwApi? = gateway?.let { GwApi(it.ip, it.token) }

    fun saveGateway(gw: Gateway?) {
        if (gw == null) store.clear() else store.save(gw)
        gateway = gw
        info = null
        nodes = emptyList()
        linkError = null
        failures = 0
    }

    fun startPolling() {
        if (pollJob?.isActive == true) return
        pollJob = viewModelScope.launch {
            while (isActive) {
                refresh()
                delay(if (ota is OtaUi.Rebooting) 1000 else 2000)
            }
        }
    }

    fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
    }

    suspend fun refresh() {
        val api = api() ?: return
        try {
            val i = api.info()
            val nl = api.nodes()
            info = i
            nodes = nl.nodes
            pairLeft = nl.pairLeft
            linkError = null
            failures = 0
            (ota as? OtaUi.Rebooting)?.let {
                if (i.version == it.version) ota = OtaUi.Done(it.version)
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            failures++
            linkError = e.userMessage()
            // 连续失败时用 mDNS 按 MAC 找一下，网关 IP 可能变了
            if (failures == 3 && e !is ApiException) rediscover()
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

    private fun action(block: suspend (GwApi) -> Unit) {
        val api = api() ?: return
        viewModelScope.launch {
            try {
                block(api)
                refresh()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                message = e.userMessage()
            }
        }
    }

    fun setChannel(node: Node, ch: Int, value: Int) = action { it.set(node.mac, ch, value) }

    fun unpair(node: Node) = action {
        it.unpair(node.mac)
        message = "已解绑 ${node.title}"
    }

    fun pair(seconds: Int) = action { it.pair(seconds) }

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
