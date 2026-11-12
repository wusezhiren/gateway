package com.homenet.espgw.data

import android.content.ComponentName
import android.content.Context
import android.service.quicksettings.TileService
import com.homenet.espgw.tile.LampTile1
import com.homenet.espgw.tile.LampTile2
import com.homenet.espgw.tile.LampTile3
import com.homenet.espgw.widget.LampWidget
import org.json.JSONObject

/** 下拉快捷开关绑定的节点通道 */
data class TileBinding(val mac: String, val ch: Int, val label: String) {
    fun toJson(): JSONObject = JSONObject().put("mac", mac).put("ch", ch).put("label", label)

    companion object {
        fun fromJson(o: JSONObject) = TileBinding(o.getString("mac"), o.getInt("ch"), o.optString("label"))
    }
}

/** 快捷开关 1..SLOTS 的绑定（SharedPreferences），每个槽位对应一个 TileService */
class QuickTiles(private val context: Context) {
    private val prefs = context.getSharedPreferences("tiles", Context.MODE_PRIVATE)

    fun get(slot: Int): TileBinding? = prefs.getString("slot$slot", null)?.let {
        runCatching { TileBinding.fromJson(JSONObject(it)) }.getOrNull()
    }

    fun set(slot: Int, binding: TileBinding?) {
        prefs.edit().apply {
            if (binding == null) remove("slot$slot") else putString("slot$slot", binding.toJson().toString())
        }.apply()
        refresh(slot)
    }

    /** 绑定了这个节点通道的槽位 */
    fun slotOf(mac: String, ch: Int): Int? = (1..SLOTS).firstOrNull { get(it)?.let { b -> b.mac == mac && b.ch == ch } == true }

    /** 让下拉栏里的磁贴重新取状态（开关在 App 里变了时调用） */
    fun refresh(slot: Int) {
        TileService.requestListeningState(context, ComponentName(context, SERVICES[slot - 1]))
    }

    fun refreshAll() = (1..SLOTS).forEach(::refresh)

    /** 节点在 App、网关屏幕或别的手机上改了名，下拉磁贴的名字跟着改 */
    fun syncLabels(nodes: List<Node>) {
        for (slot in 1..SLOTS) {
            val b = get(slot) ?: continue
            val node = nodes.firstOrNull { it.mac == b.mac } ?: continue
            val name = label(node, b.ch)
            if (name != b.label) set(slot, b.copy(label = name))
        }
    }

    companion object {
        const val SLOTS = 3

        fun label(node: Node, ch: Int) = if (node.ch.size > 1) "${node.title} ${ch + 1}" else node.title
        private val SERVICES = listOf(LampTile1::class.java, LampTile2::class.java, LampTile3::class.java)
    }
}
