package com.homenet.espgw.data

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import java.io.IOException
import java.net.Inet4Address
import kotlin.coroutines.resume

data class FoundGateway(val name: String, val ip: String, val mac: String, val version: String)

/** 通过 mDNS（_espgw._tcp）发现局域网里的网关 */
class Discovery(context: Context) {
    private val nsd = context.getSystemService(NsdManager::class.java)

    /** 持续发现，每次有变化发出当前列表；取消收集即停止 */
    fun discover(): Flow<List<FoundGateway>> = callbackFlow {
        val found = LinkedHashMap<String, FoundGateway>()
        val queue = Channel<NsdServiceInfo>(Channel.UNLIMITED)
        val listener = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(info: NsdServiceInfo) {
                queue.trySend(info)
            }

            override fun onServiceLost(info: NsdServiceInfo) {
                if (found.remove(info.serviceName) != null) {
                    trySend(found.values.toList())
                }
            }

            override fun onStartDiscoveryFailed(type: String, code: Int) {
                close(IOException("mDNS 发现失败（$code）"))
            }

            override fun onDiscoveryStarted(type: String) {}
            override fun onDiscoveryStopped(type: String) {}
            override fun onStopDiscoveryFailed(type: String, code: Int) {}
        }
        nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
        // 解析要串行：旧版 NsdManager 同时只能解析一个
        val resolver = launch {
            for (s in queue) {
                resolve(s)?.let {
                    found[it.name] = it
                    send(found.values.toList())
                }
            }
        }
        send(emptyList())
        awaitClose {
            resolver.cancel()
            runCatching { nsd.stopServiceDiscovery(listener) }
        }
    }

    /** 按 MAC 找网关（网关 IP 变了时用），找不到返回 null */
    suspend fun findByMac(mac: String, timeoutMs: Long = 8000): FoundGateway? =
        withTimeoutOrNull(timeoutMs) {
            discover().mapNotNull { list -> list.firstOrNull { it.mac.equals(mac, ignoreCase = true) } }.first()
        }

    @Suppress("DEPRECATION")
    private suspend fun resolve(s: NsdServiceInfo): FoundGateway? = withTimeoutOrNull(5000) {
        suspendCancellableCoroutine { cont ->
            nsd.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(info: NsdServiceInfo, code: Int) {
                    if (cont.isActive) cont.resume(null)
                }

                override fun onServiceResolved(info: NsdServiceInfo) {
                    val addr = if (Build.VERSION.SDK_INT >= 34) {
                        info.hostAddresses.firstOrNull { it is Inet4Address } ?: info.hostAddresses.firstOrNull()
                    } else {
                        info.host
                    }
                    val attr = info.attributes
                    val gw = addr?.hostAddress?.let { ip ->
                        FoundGateway(
                            name = info.serviceName,
                            ip = ip,
                            mac = attr["mac"]?.toString(Charsets.UTF_8) ?: "",
                            version = attr["version"]?.toString(Charsets.UTF_8) ?: "",
                        )
                    }
                    if (cont.isActive) cont.resume(gw)
                }
            })
        }
    }

    companion object {
        const val SERVICE_TYPE = "_espgw._tcp"
    }
}
