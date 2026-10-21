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
    /** 已跳去系统设置等用户允许安装，回来后自动继续 */
    private var awaitingPermission = false

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

    /** 界面回到前台时调用：从授权页回来且已允许，就接着下载安装 */
    fun onResume() {
        if (!awaitingPermission) return
        if (AppUpdate.canInstall(getApplication())) {
            install()
        } else {
            awaitingPermission = false
            error = "没有允许“安装未知应用”，无法更新"
        }
    }

    fun install() {
        val rel = latest ?: return
        val ctx = getApplication<Application>()
        if (!AppUpdate.canInstall(ctx)) {
            error = "请在接下来的系统页面里允许“安装未知应用”，然后返回"
            awaitingPermission = true
            AppUpdate.openInstallPermission(ctx)
            return
        }
        awaitingPermission = false
        error = null
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
