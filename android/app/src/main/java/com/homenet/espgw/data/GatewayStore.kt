package com.homenet.espgw.data

import android.content.Context
import org.json.JSONObject

/** 保存已绑定的网关（SharedPreferences） */
class GatewayStore(context: Context) {
    private val prefs = context.getSharedPreferences("gateway", Context.MODE_PRIVATE)

    fun load(): Gateway? = prefs.getString("gw", null)?.let {
        runCatching { Gateway.fromJson(JSONObject(it)) }.getOrNull()
    }

    fun save(gw: Gateway) {
        prefs.edit().putString("gw", gw.toJson().toString()).apply()
    }

    fun clear() {
        prefs.edit().remove("gw").apply()
    }

    fun loadCloud(): CloudConfig? = prefs.getString("cloud", null)?.let {
        runCatching { CloudConfig.fromJson(JSONObject(it)) }.getOrNull()
    }

    fun saveCloud(cfg: CloudConfig?) {
        prefs.edit().apply { if (cfg == null) remove("cloud") else putString("cloud", cfg.toJson().toString()) }.apply()
    }
}
