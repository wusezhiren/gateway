package com.homenet.espgw.data

import android.content.Context
import com.homenet.espgw.widget.LampWidget
import org.json.JSONObject

/** 桌面小组件独立绑定；旧版的前三个槽位首次读取时沿用下拉快捷开关配置。 */
class WidgetBindings(private val context: Context) {
    private val prefs = context.getSharedPreferences("widget_bindings", Context.MODE_PRIVATE)

    fun get(slot: Int): TileBinding? {
        if (slot !in 1..SLOTS) return null
        val key = "slot$slot"
        if (prefs.contains(key)) return prefs.getString(key, null)?.let {
            runCatching { TileBinding.fromJson(JSONObject(it)) }.getOrNull()
        }
        return if (slot <= QuickTiles.SLOTS) QuickTiles(context).get(slot) else null
    }

    fun set(slot: Int, binding: TileBinding?) {
        require(slot in 1..SLOTS)
        prefs.edit().putString("slot$slot", binding?.toJson()?.toString()).apply()
        LampWidget.refresh(context)
    }

    fun syncLabels(nodes: List<Node>) {
        var changed = false
        val edit = prefs.edit()
        for (slot in 1..SLOTS) {
            val binding = get(slot) ?: continue
            val node = nodes.firstOrNull { it.mac.equals(binding.mac, ignoreCase = true) } ?: continue
            val label = QuickTiles.label(node, binding.ch)
            if (binding.label != label) {
                edit.putString("slot$slot", binding.copy(label = label).toJson().toString())
                changed = true
            }
        }
        if (changed) {
            edit.apply()
            LampWidget.refresh(context)
        }
    }

    companion object { const val SLOTS = 4 }
}
