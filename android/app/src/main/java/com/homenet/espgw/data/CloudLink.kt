package com.homenet.espgw.data

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.eclipse.paho.client.mqttv3.IMqttActionListener
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken
import org.eclipse.paho.client.mqttv3.IMqttToken
import org.eclipse.paho.client.mqttv3.MqttAsyncClient
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONObject
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.coroutines.suspendCoroutine

/** 云端命令失败（网关回复的错误码，或超时） */
class CloudException(val error: String) : IOException(error)

/** 网关通过云端上报的状态（保留消息） */
data class CloudState(val info: GwInfo, val nodes: NodeList, val receivedAt: Long)

/**
 * 通过云端 MQTT 访问网关，协议见 docs/cloud.md：
 * 订阅 espgw/<id>/state、online、resp；命令发到 espgw/<id>/cmd，带 API token。
 */
class CloudLink(
    private val cfg: CloudConfig,
    private val gwId: String,
    private val token: String,
    private val scope: CoroutineScope,
) {
    private val topicState = "espgw/$gwId/state"
    private val topicOnline = "espgw/$gwId/online"
    private val topicCmd = "espgw/$gwId/cmd"
    private val topicResp = "espgw/$gwId/resp"

    private val _connected = MutableStateFlow(false)
    /** 手机到云端的连接 */
    val connected: StateFlow<Boolean> = _connected

    private val _gwOnline = MutableStateFlow<Boolean?>(null)
    /** 网关到云端的连接（null：还没收到） */
    val gwOnline: StateFlow<Boolean?> = _gwOnline

    private val _state = MutableStateFlow<CloudState?>(null)
    val state: StateFlow<CloudState?> = _state

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val pending = ConcurrentHashMap<String, CompletableDeferred<JSONObject>>()
    private val client = MqttAsyncClient(
        "ssl://${cfg.host}:${cfg.port}",
        "espgw-app-" + UUID.randomUUID().toString().take(8),
        MemoryPersistence(),
    )
    private var connectJob: Job? = null

    fun start() {
        client.setCallback(object : MqttCallbackExtended {
            override fun connectComplete(reconnect: Boolean, serverURI: String?) {
                _connected.value = true
                _error.value = null
                runCatching {
                    client.subscribe(arrayOf(topicState, topicOnline, topicResp), intArrayOf(1, 1, 1))
                }
            }

            override fun connectionLost(cause: Throwable?) {
                _connected.value = false
            }

            override fun messageArrived(topic: String, message: MqttMessage) {
                val text = message.payload.toString(Charsets.UTF_8)
                when (topic) {
                    topicOnline -> _gwOnline.value = text == "1"
                    topicState -> runCatching {
                        val o = JSONObject(text)
                        _state.value = CloudState(
                            GwInfo.parse(o.getJSONObject("info")),
                            NodeList.parse(o.getJSONObject("nodes")),
                            System.currentTimeMillis(),
                        )
                    }
                    topicResp -> runCatching {
                        val o = JSONObject(text)
                        pending.remove(o.optString("id"))?.complete(o)
                    }
                }
            }

            override fun deliveryComplete(token: IMqttDeliveryToken?) {}
        })
        // 首次连接失败时 Paho 不会自动重试，自己循环；连上之后由自动重连接管
        connectJob = scope.launch(Dispatchers.IO) {
            while (isActive && !client.isConnected) {
                try {
                    connectOnce()
                } catch (e: Exception) {
                    _error.value = e.message ?: e.javaClass.simpleName
                    delay(5000)
                }
            }
        }
    }

    private suspend fun connectOnce() = suspendCoroutine { cont ->
        val opts = MqttConnectOptions().apply {
            userName = cfg.username
            password = cfg.password.toCharArray()
            isAutomaticReconnect = true
            isCleanSession = true
            keepAliveInterval = 30
            connectionTimeout = 10
            // 默认最长 128 秒，切换网络后会等很久
            maxReconnectDelay = 10_000
        }
        client.connect(opts, null, object : IMqttActionListener {
            override fun onSuccess(asyncActionToken: IMqttToken?) = cont.resume(Unit)
            override fun onFailure(asyncActionToken: IMqttToken?, exception: Throwable?) =
                cont.resumeWithException(exception ?: IOException("连接云端失败"))
        })
    }

    /** 发送命令并等待网关回复；失败抛 CloudException */
    suspend fun command(op: String, args: JSONObject = JSONObject()) {
        if (!client.isConnected) throw CloudException("cloud_disconnected")
        if (_gwOnline.value == false) throw CloudException("gateway_offline")
        val id = UUID.randomUUID().toString().take(12)
        val waiter = CompletableDeferred<JSONObject>()
        pending[id] = waiter
        try {
            args.put("id", id).put("token", token).put("op", op)
            withContext(Dispatchers.IO) {
                client.publish(topicCmd, MqttMessage(args.toString().toByteArray()).apply { qos = 1 })
            }
            val r = withTimeoutOrNull(8000) { waiter.await() } ?: throw CloudException("timeout")
            if (!r.optBoolean("ok")) throw CloudException(r.optString("error", "failed"))
        } finally {
            pending.remove(id)
        }
    }

    fun close() {
        connectJob?.cancel()
        runCatching { client.disconnectForcibly(0, 0) }
        runCatching { client.close(true) }
        _connected.value = false
    }
}
