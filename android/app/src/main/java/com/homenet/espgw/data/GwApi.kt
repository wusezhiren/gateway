package com.homenet.espgw.data

import android.net.Network
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.Proxy
import java.net.URL

/** 网关返回的错误：HTTP 状态码 + {"error": "..."} */
class ApiException(val code: Int, val error: String) : IOException("HTTP $code $error")

/**
 * 网关 HTTP 接口（docs/api.md）。
 * network 不为空时请求走指定网络（配网时绑定到网关热点，不影响手机默认网络）。
 * 局域网设备一律不走代理。
 */
class GwApi(
    private val host: String,
    private val token: String? = null,
    private val network: Network? = null,
) {
    private fun open(path: String, timeoutMs: Int): HttpURLConnection {
        val url = URL("http://$host$path")
        val c = (network?.openConnection(url) ?: url.openConnection(Proxy.NO_PROXY)) as HttpURLConnection
        c.connectTimeout = timeoutMs
        c.readTimeout = timeoutMs
        c.useCaches = false
        token?.let { c.setRequestProperty("Authorization", "Bearer $it") }
        return c
    }

    private fun readBody(c: HttpURLConnection, code: Int): String {
        val s = if (code >= 400) c.errorStream else c.inputStream
        return s?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
    }

    private fun check(code: Int, body: String) {
        if (code !in 200..299) {
            val err = runCatching { JSONObject(body).optString("error") }.getOrNull()
            throw ApiException(code, err?.ifBlank { null } ?: "http_$code")
        }
    }

    private suspend fun request(
        method: String,
        path: String,
        body: JSONObject? = null,
        timeoutMs: Int = 4000,
    ): String = withContext(Dispatchers.IO) {
        val c = open(path, timeoutMs)
        try {
            c.requestMethod = method
            if (body != null) {
                val bytes = body.toString().toByteArray()
                c.doOutput = true
                c.setRequestProperty("Content-Type", "application/json")
                c.setFixedLengthStreamingMode(bytes.size)
                c.outputStream.use { it.write(bytes) }
            }
            val code = c.responseCode
            val text = readBody(c, code)
            check(code, text)
            text
        } finally {
            c.disconnect()
        }
    }

    suspend fun info(): GwInfo = GwInfo.parse(JSONObject(request("GET", "/api/info")))

    suspend fun nodes(): NodeList = NodeList.parse(JSONObject(request("GET", "/api/nodes")))

    /** 把云端 broker 配置下发给网关（host 为空表示关闭） */
    suspend fun setCloud(cfg: CloudConfig) {
        request("POST", "/api/cloud", cfg.toJson())
    }

    suspend fun set(mac: String, ch: Int, value: Int) {
        request("POST", "/api/nodes/set", JSONObject().put("mac", mac).put("ch", ch).put("value", value))
    }

    suspend fun unpair(mac: String) {
        request("POST", "/api/nodes/unpair", JSONObject().put("mac", mac))
    }

    suspend fun pair(seconds: Int) {
        request("POST", "/api/pair", JSONObject().put("seconds", seconds))
    }

    /** 发起绑定，返回请求 id；需要在网关屏幕上点“允许” */
    suspend fun bindStart(phoneName: String): String =
        JSONObject(request("POST", "/api/bind", JSONObject().put("name", phoneName))).getString("id")

    suspend fun bindPoll(id: String): BindResult {
        val o = JSONObject(request("GET", "/api/bind?id=$id"))
        return BindResult(o.optString("state"), o.optString("token"))
    }

    // ---------- 配网（仅在网关热点内可用） ----------

    suspend fun provScan(): List<ScanAp> {
        val arr = JSONArray(request("GET", "/api/prov/scan", timeoutMs = 8000))
        return (0 until arr.length()).map {
            val o = arr.getJSONObject(it)
            ScanAp(o.optString("ssid"), o.optInt("rssi"), o.optBoolean("secure", true))
        }
    }

    suspend fun provConnect(ssid: String, password: String) {
        request("POST", "/api/prov/connect", JSONObject().put("ssid", ssid).put("password", password))
    }

    suspend fun provStatus(): ProvStatus {
        val o = JSONObject(request("GET", "/api/prov/status"))
        return ProvStatus(
            state = o.optString("state"),
            ssid = o.optString("ssid"),
            reason = o.optString("reason"),
            ip = o.optString("ip"),
            token = o.optString("token"),
            closeIn = o.optInt("close_in"),
        )
    }

    // ---------- 网关 OTA ----------

    /** 流式上传签名后的网关固件，返回目标版本 */
    suspend fun otaGateway(
        input: InputStream,
        length: Long,
        force: Boolean,
        progress: (Long) -> Unit,
    ): String = withContext(Dispatchers.IO) {
        val c = open("/api/ota/gateway" + if (force) "?force=1" else "", 30000)
        try {
            c.requestMethod = "POST"
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/octet-stream")
            c.setFixedLengthStreamingMode(length)
            c.outputStream.use { out ->
                val buf = ByteArray(4096)
                var sent = 0L
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    sent += n
                    progress(sent)
                }
            }
            val code = c.responseCode
            val text = readBody(c, code)
            check(code, text)
            JSONObject(text).optString("version")
        } finally {
            c.disconnect()
        }
    }
}

/** 错误码转成给用户看的文字 */
fun Throwable.userMessage(): String = when (this) {
    is ApiException -> when (error) {
        "unauthorized" -> "网关拒绝访问（token 无效，请重新绑定）"
        "offline" -> "节点离线"
        "no_such_node" -> "节点不存在"
        "busy" -> "网关忙，请稍后重试"
        "not_newer" -> "版本不高于网关当前版本"
        "wrong_project" -> "不是网关固件"
        "not_an_app_image" -> "不是 ESP 固件文件"
        "invalid_image_or_signature" -> "固件损坏或签名不对"
        "too_large" -> "固件太大"
        "not_provisioning" -> "网关不在配网模式"
        "invalid_ssid_or_password" -> "Wi-Fi 名称或密码格式不对（密码 8~63 位）"
        else -> "网关错误：$error"
    }
    is CloudException -> when (error) {
        "unauthorized" -> "网关拒绝访问（token 无效，请重新绑定）"
        "offline" -> "节点离线"
        "no_such_node" -> "节点不存在"
        "timeout" -> "网关没有响应（云端）"
        "gateway_offline" -> "网关不在线"
        else -> "网关错误：$error"
    }
    is java.net.SocketTimeoutException -> "连接网关超时"
    is java.net.ConnectException, is java.net.NoRouteToHostException -> "连不上网关"
    else -> message ?: javaClass.simpleName
}
