package com.homenet.espgw.widget

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.view.View
import android.widget.RemoteViews
import com.homenet.espgw.MainActivity
import com.homenet.espgw.R
import com.homenet.espgw.data.QuickTiles
import com.homenet.espgw.data.WidgetBindings
import com.homenet.espgw.data.QuickStateCache
import com.homenet.espgw.data.Node
import com.homenet.espgw.data.Rgb
import com.homenet.espgw.data.SwitchClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * 桌面小组件：独立的 4 个槽位，前三个沿用旧版快捷开关绑定直到用户修改。
 * 状态在系统定时更新（30 分钟）、点击、App 退到后台、改绑定时刷新。
 */
class LampWidget : AppWidgetProvider() {

    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        work(context) { scope ->
            val widgetBindings = WidgetBindings(context)
            val bindings = (1..WidgetBindings.SLOTS).map(widgetBindings::get)
            val cache = QuickStateCache(context)
            render(context, bindings.map { b ->
                if (b == null) null else cache.value(b)?.let { if (it != 0) Shown.ON else Shown.OFF }
                    ?: Shown.LOADING
            }, keep = true)
            val bound = bindings.filterNotNull()
            val states = if (bound.isEmpty()) emptyList()
            else withTimeoutOrNull(BUDGET_MS) { runCatching { SwitchClient(context, scope).states(bound) }.getOrNull() }
            val iter = states?.iterator()
            render(context, bindings.map { b ->
                when {
                    b == null -> null
                    iter == null -> Shown.UNKNOWN
                    else -> when (iter.next()) {
                        true -> Shown.ON
                        false -> Shown.OFF
                        null -> Shown.UNKNOWN
                    }
                }
            })
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_TOGGLE) {
            super.onReceive(context, intent)
            return
        }
        val slot = intent.getIntExtra(EXTRA_SLOT, 0)
        val b = WidgetBindings(context).get(slot) ?: return refresh(context)
        work(context) { scope ->
            setSlot(context, slot, Shown.LOADING)
            val on = withTimeoutOrNull(BUDGET_MS) { runCatching { SwitchClient(context, scope).toggle(b) }.getOrNull() }
            setSlot(context, slot, when (on) {
                true -> Shown.ON
                false -> Shown.OFF
                null -> Shown.UNKNOWN
            })
            QuickTiles(context).slotOf(b.mac, b.ch)?.let { QuickTiles(context).refresh(it) }
        }
    }

    /** 广播接收器最多活 10 秒：goAsync 后在协程里做完再结束 */
    private fun work(context: Context, block: suspend (CoroutineScope) -> Unit) {
        val pending = goAsync()
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
        scope.launch {
            try {
                block(scope)
            } finally {
                pending.finish()
                scope.cancel()
            }
        }
    }

    private enum class Shown { ON, OFF, LOADING, UNKNOWN }

    companion object {
        private const val ACTION_TOGGLE = "com.homenet.espgw.widget.TOGGLE"
        private const val EXTRA_SLOT = "slot"
        private const val BUDGET_MS = 9_000L
        private val BUTTONS = intArrayOf(R.id.btn1, R.id.btn2, R.id.btn3, R.id.btn4)

        /** 状态缓存：只改一个按钮时其他按钮照旧显示 */
        private val shown = arrayOfNulls<Shown>(WidgetBindings.SLOTS)

        /** 重新取状态（改了绑定、App 里改了开关后调用） */
        fun refresh(context: Context) {
            val mgr = AppWidgetManager.getInstance(context)
            val ids = mgr.getAppWidgetIds(ComponentName(context, LampWidget::class.java))
            if (ids.isEmpty()) return
            context.sendBroadcast(
                Intent(context, LampWidget::class.java)
                    .setAction(AppWidgetManager.ACTION_APPWIDGET_UPDATE)
                    .putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, ids)
            )
        }

        /** Foreground HTTP poll or MQTT push already has fresh data; avoid another request. */
        fun showNodes(context: Context, nodes: List<Node>) {
            val bindings = WidgetBindings(context)
            render(context, (1..WidgetBindings.SLOTS).map { slot ->
                val b = bindings.get(slot) ?: return@map null
                val channel = nodes.firstOrNull { it.mac.equals(b.mac, ignoreCase = true) }
                    ?.ch?.getOrNull(b.ch) ?: return@map Shown.UNKNOWN
                val value = channel.desired ?: channel.value
                val on = if (channel.type == "rgb") Rgb.on(value) else value != 0
                if (on) Shown.ON else Shown.OFF
            })
        }

        private fun setSlot(context: Context, slot: Int, s: Shown) {
            val all = shown.copyOf()
            all[slot - 1] = s
            render(context, all.toList())
        }

        /** keep：LOADING 时保留上次的开关颜色，只改文字，避免每次刷新都闪一下 */
        private fun render(context: Context, states: List<Shown?>, keep: Boolean = false) {
            val tiles = WidgetBindings(context)
            val views = RemoteViews(context.packageName, R.layout.widget_lamp)
            var any = false
            for (slot in 1..WidgetBindings.SLOTS) {
                val id = BUTTONS[slot - 1]
                val b = tiles.get(slot)
                var s = states[slot - 1]
                if (b == null || s == null) {
                    views.setViewVisibility(id, View.GONE)
                    shown[slot - 1] = null
                    continue
                }
                any = true
                if (keep && s == Shown.LOADING && shown[slot - 1] in listOf(Shown.ON, Shown.OFF)) {
                    s = shown[slot - 1]!!
                }
                shown[slot - 1] = s
                val status = when (s) {
                    Shown.ON -> "开"
                    Shown.OFF -> "关"
                    Shown.LOADING -> "…"
                    Shown.UNKNOWN -> "连不上"
                }
                views.setViewVisibility(id, View.VISIBLE)
                views.setTextViewText(id, "${b.label}\n$status")
                views.setInt(id, "setBackgroundResource", when (s) {
                    Shown.ON -> R.drawable.widget_btn_on
                    Shown.OFF -> R.drawable.widget_btn_off
                    else -> R.drawable.widget_btn_unknown
                })
                val toggle = Intent(context, LampWidget::class.java).setAction(ACTION_TOGGLE).putExtra(EXTRA_SLOT, slot)
                views.setOnClickPendingIntent(id, PendingIntent.getBroadcast(
                    context, slot, toggle, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
            }
            views.setViewVisibility(R.id.empty, if (any) View.GONE else View.VISIBLE)
            if (!any) {
                val open = Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                views.setOnClickPendingIntent(R.id.root, PendingIntent.getActivity(context, 0, open, PendingIntent.FLAG_IMMUTABLE))
            }
            val mgr = AppWidgetManager.getInstance(context)
            mgr.updateAppWidget(ComponentName(context, LampWidget::class.java), views)
        }
    }
}
