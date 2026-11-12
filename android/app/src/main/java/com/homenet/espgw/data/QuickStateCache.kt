package com.homenet.espgw.data

import android.content.Context

/** Last confirmed node snapshot shared by the foreground UI and background shortcuts. */
class QuickStateCache(context: Context) {
    private val prefs = context.getSharedPreferences("quick_state", Context.MODE_PRIVATE)

    fun update(nodes: List<Node>) {
        val edit = prefs.edit().clear()
        nodes.forEach { node ->
            node.ch.forEachIndexed { index, channel ->
                edit.putInt("${node.mac.lowercase()}:$index", channel.desired ?: channel.value)
            }
        }
        edit.putLong("updated", System.currentTimeMillis()).apply()
    }

    fun value(binding: TileBinding): Int? {
        if (System.currentTimeMillis() - prefs.getLong("updated", 0) !in 0..MAX_AGE_MS) return null
        val key = "${binding.mac.lowercase()}:${binding.ch}"
        return if (prefs.contains(key)) prefs.getInt(key, 0) else null
    }

    fun setRelay(binding: TileBinding, value: Int) {
        prefs.edit().putInt("${binding.mac.lowercase()}:${binding.ch}", value).apply()
    }

    fun clear() {
        prefs.edit().clear().apply()
    }

    companion object {
        private const val MAX_AGE_MS = 5_000L
    }
}
