package com.homenet.espgw.tile

import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import com.homenet.espgw.MainActivity
import com.homenet.espgw.data.GatewayStore
import com.homenet.espgw.data.QuickTiles
import com.homenet.espgw.data.SwitchClient
import com.homenet.espgw.data.userMessage
import com.homenet.espgw.widget.LampWidget
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/** 下拉快捷开关：一个槽位绑定一个节点通道（App 节点详情里设置），点一下取反 */
abstract class LampTileService(private val slot: Int) : TileService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null

    override fun onStartListening() {
        val b = QuickTiles(this).get(slot)
        if (b == null) {
            show(Tile.STATE_UNAVAILABLE, "灯 $slot", "在 App 节点详情里设置")
            return
        }
        show(qsTile?.state ?: Tile.STATE_INACTIVE, b.label, null)
        job?.cancel()
        job = scope.launch {
            runCatching { SwitchClient(this@LampTileService, scope).state(b) }
                .onSuccess { show(if (it) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE, b.label, null) }
                .onFailure { show(Tile.STATE_UNAVAILABLE, b.label, it.userMessage()) }
        }
    }

    override fun onClick() {
        val b = QuickTiles(this).get(slot)
        if (b == null || GatewayStore(this).load() == null) {
            openApp()
            return
        }
        val wasOn = qsTile?.state == Tile.STATE_ACTIVE
        show(if (wasOn) Tile.STATE_INACTIVE else Tile.STATE_ACTIVE, b.label, "…")
        job?.cancel()
        job = scope.launch {
            runCatching { SwitchClient(this@LampTileService, scope).toggle(b) }.onSuccess {
                LampWidget.refresh(this@LampTileService)
            }.onSuccess { show(if (it) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE, b.label, null) }
                .onFailure { show(if (wasOn) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE, b.label, it.userMessage()) }
        }
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    private fun show(state: Int, label: String, subtitle: String?) {
        val t = qsTile ?: return
        t.state = state
        t.label = label
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            t.subtitle = subtitle ?: when (state) {
                Tile.STATE_ACTIVE -> "开"
                Tile.STATE_INACTIVE -> "关"
                else -> null
            }
        }
        t.updateTile()
    }

    private fun openApp() {
        val intent = Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE))
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(intent)
        }
    }

}

class LampTile1 : LampTileService(1)
class LampTile2 : LampTileService(2)
class LampTile3 : LampTileService(3)
