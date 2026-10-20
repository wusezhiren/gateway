package com.homenet.espgw.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.PatternMatcher
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * App 内连接网关配网热点（ESP-GW-XXXX）。
 * 系统会弹出选择框；连上后只有本 App 的请求走这个网络（不影响手机默认网络）。
 * 网关连上路由器时热点会切信道，network 可能短暂变成 null 再恢复。
 */
class Hotspot(context: Context) {
    private val cm = context.getSystemService(ConnectivityManager::class.java)
    private var callback: ConnectivityManager.NetworkCallback? = null

    private val _network = MutableStateFlow<Network?>(null)
    val network: StateFlow<Network?> = _network

    /** 系统选择框被取消或超时 */
    private val _unavailable = MutableStateFlow(false)
    val unavailable: StateFlow<Boolean> = _unavailable

    fun request() {
        release()
        _unavailable.value = false
        val spec = WifiNetworkSpecifier.Builder()
            .setSsidPattern(PatternMatcher(SSID_PREFIX, PatternMatcher.PATTERN_PREFIX))
            .setWpa2Passphrase(PASSWORD)
            .build()
        val req = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(spec)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                _network.value = network
            }

            override fun onLost(network: Network) {
                if (_network.value == network) _network.value = null
            }

            override fun onUnavailable() {
                _unavailable.value = true
            }
        }
        callback = cb
        cm.requestNetwork(req, cb, 120_000)
    }

    fun release() {
        callback?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        callback = null
        _network.value = null
    }

    companion object {
        const val SSID_PREFIX = "ESP-GW-"
        /** 与网关 CONFIG_GW_PROV_AP_PASSWORD 一致 */
        const val PASSWORD = "a12345678"

        /** 手机当前连接的 Wi-Fi 名称；需要定位权限且定位开关打开，否则返回 null */
        @Suppress("DEPRECATION")
        fun currentSsid(context: Context): String? {
            val wm = context.applicationContext.getSystemService(WifiManager::class.java)
            val ssid = wm.connectionInfo?.ssid ?: return null
            if (ssid == WifiManager.UNKNOWN_SSID || ssid.isBlank()) return null
            return ssid.removeSurrounding("\"").takeUnless { it.startsWith(SSID_PREFIX) }
        }
    }
}
