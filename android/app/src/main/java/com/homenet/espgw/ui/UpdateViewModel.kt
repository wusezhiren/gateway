package com.homenet.espgw.ui

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.homenet.espgw.BuildConfig
import com.homenet.espgw.data.AppRelease
import com.homenet.espgw.data.AppUpdate
import com.homenet.espgw.data.userMessage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import java.io.File

/** App 自更新：启动时、收到云端通知时、手动点“检查更新”时检查 */
class UpdateViewModel(app: Application) : AndroidViewModel(app) {
    var latest by mutableStateOf<AppRelease?>(null)
        private set
    /** 弹出更新对话框 */
    var showDialog by mutableStateOf(false)
    var checking by mutableStateOf(false)
        private set
    var progress by mutableStateOf<Float?>(null)
        private set
    var error by mutableStateOf<String?>(null)
    /** 一次性提示（如“已是最新版本”），由界面显示后清掉 */
    var message by mutableStateOf<String?>(null)

    private var downloaded: File? = null
    private var dismissedCode = 0

    val currentVersion: String get() = BuildConfig.VERSION_NAME

    fun check(manual: Boolean) {
        if (checking) return
        viewModelScope.launch {
            checking = true
            try {
                val rel = AppUpdate.fetchLatest()
                latest = rel
                if (rel.versionCode > BuildConfig.VERSION_CODE) {
                    if (manual || rel.versionCode != dismissedCode) showDialog = true
                } else if (manual) {
                    message = "已是最新版本 v${BuildConfig.VERSION_NAME}"
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (manual) message = "检查更新失败：${e.userMessage()}"
            } finally {
                checking = false
            }
        }
    }

    fun dismiss() {
        showDialog = false
        latest?.let { dismissedCode = it.versionCode }
    }

    fun install() {
        val rel = latest ?: return
        val ctx = getApplication<Application>()
        if (!AppUpdate.canInstall(ctx)) {
            error = "请在接下来的系统页面里允许“安装未知应用”，然后返回再点一次“更新”"
            AppUpdate.openInstallPermission(ctx)
            return
        }
        downloaded?.takeIf { it.exists() }?.let {
            AppUpdate.install(ctx, it)
            return
        }
        if (progress != null) return
        viewModelScope.launch {
            error = null
            progress = 0f
            try {
                val f = AppUpdate.download(ctx, rel) { got, total ->
                    progress = if (total > 0) got.toFloat() / total else 0f
                }
                downloaded = f
                AppUpdate.install(ctx, f)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.userMessage()
            } finally {
                progress = null
            }
        }
    }
}
