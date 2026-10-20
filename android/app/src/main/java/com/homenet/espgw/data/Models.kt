package com.homenet.espgw.data

import org.json.JSONArray
import org.json.JSONObject

/** 手机里保存的网关（目前只支持一个） */
data class Gateway(
    val mac: String,
    val ip: String,
    val token: String,
    val name: String,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("mac", mac).put("ip", ip).put("token", token).put("name", name)

    companion object {
        fun fromJson(o: JSONObject) = Gateway(
            mac = o.getString("mac"),
            ip = o.getString("ip"),
            token = o.getString("token"),
            name = o.optString("name", "ESP 网关"),
        )
    }
}

data class OtaStatus(
    val state: String,
    val received: Long,
    val total: Long,
    val version: String,
    val error: String,
)

/** GET /api/info */
data class GwInfo(
    val project: String,
    val version: String,
    val partition: String,
    val idf: String,
    val mac: String,
    val uptimeS: Long,
    val wifiSsid: String,
    val wifiConnected: Boolean,
    val ip: String,
    val rssi: Int,
    val channel: Int,
    val nodesTotal: Int,
    val nodesOnline: Int,
    val pairLeft: Int,
    val ota: OtaStatus,
) {
    companion object {
        fun parse(o: JSONObject): GwInfo {
            val w = o.optJSONObject("wifi") ?: JSONObject()
            val n = o.optJSONObject("nodes") ?: JSONObject()
            val ota = o.optJSONObject("ota") ?: JSONObject()
            return GwInfo(
                project = o.optString("project"),
                version = o.optString("version"),
                partition = o.optString("partition"),
                idf = o.optString("idf"),
                mac = o.optString("mac"),
                uptimeS = o.optLong("uptime_s"),
                wifiSsid = w.optString("ssid"),
                wifiConnected = w.optBoolean("connected"),
                ip = w.optString("ip"),
                rssi = w.optInt("rssi"),
                channel = w.optInt("channel"),
                nodesTotal = n.optInt("total"),
                nodesOnline = n.optInt("online"),
                pairLeft = n.optInt("pair_left"),
                ota = OtaStatus(
                    state = ota.optString("state", "idle"),
                    received = ota.optLong("received"),
                    total = ota.optLong("total"),
                    version = ota.optString("version"),
                    error = ota.optString("error"),
                ),
            )
        }
    }
}

data class Channel(
    val type: String,       // relay / pwm
    val max: Int,
    val value: Int,
    val desired: Int?,      // 网关记录的期望值，与 value 不同说明还没生效
)

/** GET /api/nodes 中的一项 */
data class Node(
    val mac: String,
    val name: String,
    val online: Boolean,
    val rssi: Int,
    val uptimeS: Long,
    val lastSeenS: Long,
    val version: String,
    val project: String,
    val chip: String,
    val ch: List<Channel>,
) {
    val title: String get() = name.ifBlank { mac }

    companion object {
        fun parse(o: JSONObject): Node {
            val chs = o.optJSONArray("ch") ?: JSONArray()
            return Node(
                mac = o.optString("mac"),
                name = o.optString("name"),
                online = o.optBoolean("online"),
                rssi = o.optInt("rssi"),
                uptimeS = o.optLong("uptime_s"),
                lastSeenS = o.optLong("last_seen_s", -1),
                version = o.optString("version"),
                project = o.optString("project"),
                chip = o.optString("chip"),
                ch = (0 until chs.length()).map { i ->
                    val c = chs.getJSONObject(i)
                    Channel(
                        type = c.optString("type"),
                        max = c.optInt("max", 1),
                        value = c.optInt("value"),
                        desired = if (c.has("desired")) c.optInt("desired") else null,
                    )
                },
            )
        }
    }
}

data class NodeList(val pairLeft: Int, val nodes: List<Node>)

/** 配网热点内 GET /api/prov/scan 的一项 */
data class ScanAp(val ssid: String, val rssi: Int, val secure: Boolean)

/** GET /api/prov/status */
data class ProvStatus(
    val state: String,      // waiting / client / connecting / done / failed / off
    val ssid: String,
    val reason: String,
    val ip: String,
    val token: String,
    val closeIn: Int,
)

data class BindResult(val state: String, val token: String)
