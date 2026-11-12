package com.homenet.espgw.data

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import java.io.IOException

/**
 * 下拉快捷开关和桌面小组件用：不开 App 读取 / 切换绑定的节点通道。
 * 先走局域网（1.5 秒超时），连不上再临时连一次云端，用完断开。
 */
class SwitchClient(private val context: Context, private val scope: CoroutineScope) {

    /** 各绑定当前是否开着；节点或通道不存在的为 null */
    suspend fun states(bindings: List<TileBinding>): List<Boolean?> = withGateway { gw ->
        val nodes = gw.nodes()
        bindings.map { b -> runCatching { isOn(nodes.channelOf(b)) }.getOrNull() }
    }

    suspend fun state(b: TileBinding): Boolean = withGateway { isOn(it.nodes().channelOf(b)) }

    /** 取反，返回新的开关状态 */
    suspend fun toggle(b: TileBinding): Boolean = withGateway { gw ->
        val cached = QuickStateCache(context).value(b)
        val ch = if (cached == null) gw.nodes().channelOf(b) else null
        val v = if (ch != null) toggled(ch) else if (cached == 0) 1 else 0
        gw.set(b, v)
        if (cached != null) QuickStateCache(context).setRelay(b, v)
        if (ch != null && ch.type == "rgb") isOn(ch.copy(value = v, desired = v)) else v != 0
    }

    private interface Gw {
        suspend fun nodes(): List<Node>
        suspend fun set(b: TileBinding, value: Int)
    }

    private suspend fun <T> withGateway(block: suspend (Gw) -> T): T {
        val store = GatewayStore(context)
        val gw = store.load() ?: throw IOException("还没有绑定网关")
        val lan = GwApi(gw.ip, gw.token, timeoutMs = LAN_TIMEOUT_MS)
        val lanError = try {
            return block(object : Gw {
                override suspend fun nodes() = lan.nodes().nodes
                override suspend fun set(b: TileBinding, value: Int) = lan.set(b.mac, b.ch, value)
            })
        } catch (e: ApiException) {
            throw e         // 网关回了错误，换云端也一样
        } catch (e: IOException) {
            e
        }
        val cfg = store.loadCloud() ?: throw lanError
        val link = CloudLink(cfg, gw.cloudId, gw.token, scope)
        try {
            link.start()
            withTimeout(CLOUD_TIMEOUT_MS) {
                link.connected.first { it }
                if (link.gwOnline.filterNotNull().first() != true) throw CloudException("gateway_offline")
            }
            return block(object : Gw {
                override suspend fun nodes() =
                    withTimeout(CLOUD_TIMEOUT_MS) { link.state.filterNotNull().first() }.nodes.nodes

                override suspend fun set(b: TileBinding, value: Int) =
                    link.command("set", JSONObject().put("mac", b.mac).put("ch", b.ch).put("value", value))
            })
        } finally {
            link.close()
        }
    }

    companion object {
        private const val LAN_TIMEOUT_MS = 1500
        private const val CLOUD_TIMEOUT_MS = 8_000L

        private fun List<Node>.channelOf(b: TileBinding): Channel {
            val node = firstOrNull { it.mac.equals(b.mac, ignoreCase = true) } ?: throw IOException("节点已解绑")
            return node.ch.getOrNull(b.ch) ?: throw IOException("节点没有这一路")
        }

        /** 深睡节点的命令要等它醒来才生效，以期望值为准 */
        private fun isOn(ch: Channel): Boolean {
            val v = ch.desired ?: ch.value
            return if (ch.type == "rgb") Rgb.on(v) else v != 0
        }

        private fun toggled(ch: Channel): Int {
            val v = ch.desired ?: ch.value
            return when (ch.type) {
                "rgb" -> v xor Rgb.ON
                else -> if (v != 0) 0 else if (ch.type == "pwm") ch.max else 1
            }
        }
    }
}
