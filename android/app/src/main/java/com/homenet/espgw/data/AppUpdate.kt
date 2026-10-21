package com.homenet.espgw.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/** 同一次发布的节点固件，按工程名 + 芯片区分（不同硬件配置的板子工程名不同） */
data class NodeFirmware(
    val project: String,
    val chip: String,
    val version: String,
    val url: String,
    val sha256: String,
) {
    /** 节点上报的工程名在协议里截断为 16 个字符 */
    fun fits(projectOfNode: String, chipOfNode: String) =
        chip == chipOfNode && project.take(16) == projectOfNode
}

/** 发布仓库里 version.json 描述的最新版本 */
data class AppRelease(
    val versionCode: Int,
    val versionName: String,
    val url: String,
    val sha256: String,
    val size: Long,
    val notes: String,
    val gatewayVersion: String,
    /** 同一次发布的网关固件，远程升级时网关自己下载 */
    val gatewayUrl: String,
    val gatewaySha256: String,
    val nodes: List<NodeFirmware>,
) {
    companion object {
        fun parse(o: JSONObject) = AppRelease(
            versionCode = o.getInt("versionCode"),
            versionName = o.getString("versionName"),
            url = o.getString("url"),
            sha256 = o.getString("sha256").lowercase(),
            size = o.optLong("size"),
            notes = o.optString("notes"),
            gatewayVersion = o.optString("gatewayVersion"),
            gatewayUrl = o.optString("gatewayUrl"),
            gatewaySha256 = o.optString("gatewaySha256").lowercase(),
            nodes = o.optJSONArray("nodes")?.let { arr ->
                (0 until arr.length()).map { i ->
                    val n = arr.getJSONObject(i)
                    NodeFirmware(
                        project = n.optString("project"),
                        chip = n.optString("chip"),
                        version = n.optString("version"),
                        url = n.optString("url"),
                        sha256 = n.optString("sha256").lowercase(),
                    )
                }
            }.orEmpty(),
        )
    }
}

/**
 * App 自更新：从公开的 gitee 发布仓库读 version.json，下载 APK，校验 SHA-256 后调起系统安装。
 * 安装包签名由系统校验，必须与已安装版本一致。
 */
object AppUpdate {
    const val MANIFEST_URL = "https://gitee.com/yu_ji_hai/app_release/raw/master/version.json"

    private fun open(url: String, timeoutMs: Int): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = timeoutMs
            readTimeout = timeoutMs
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "EspGateway-App")
        }

    suspend fun fetchLatest(): AppRelease = withContext(Dispatchers.IO) {
        val c = open(MANIFEST_URL + "?t=" + System.currentTimeMillis(), 8000)
        try {
            if (c.responseCode != 200) throw IOException("检查更新失败（HTTP ${c.responseCode}）")
            AppRelease.parse(JSONObject(c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }))
        } finally {
            c.disconnect()
        }
    }

    /** 下载到缓存目录并校验 SHA-256，返回安装包文件 */
    suspend fun download(ctx: Context, rel: AppRelease, progress: (Long, Long) -> Unit): File =
        withContext(Dispatchers.IO) {
            val dir = File(ctx.cacheDir, "update").apply { mkdirs() }
            dir.listFiles()?.forEach { it.delete() }
            val file = File(dir, "EspGateway-${rel.versionName}.apk")
            var url = rel.url
            var c = open(url, 15000)
            // gitee 的附件下载会跳转，跨域名跳转 HttpURLConnection 不会自动跟随
            repeat(5) {
                if (c.responseCode !in 300..399) return@repeat
                url = URL(URL(url), c.getHeaderField("Location")).toString()
                c.disconnect()
                c = open(url, 15000)
            }
            try {
                if (c.responseCode != 200) throw IOException("下载失败（HTTP ${c.responseCode}）")
                val total = c.contentLengthLong.takeIf { it > 0 } ?: rel.size
                val md = MessageDigest.getInstance("SHA-256")
                var got = 0L
                c.inputStream.use { input ->
                    file.outputStream().use { out ->
                        val buf = ByteArray(16384)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            md.update(buf, 0, n)
                            got += n
                            progress(got, total)
                        }
                    }
                }
                val hex = md.digest().joinToString("") { "%02x".format(it) }
                if (hex != rel.sha256) {
                    file.delete()
                    throw IOException("安装包校验失败，请重试")
                }
                file
            } finally {
                c.disconnect()
            }
        }

    /** 是否已允许本 App 安装应用（Android 8+ 需要用户在系统设置里打开一次） */
    fun canInstall(ctx: Context): Boolean = ctx.packageManager.canRequestPackageInstalls()

    fun openInstallPermission(ctx: Context) {
        ctx.startActivity(
            Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}"))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }

    fun install(ctx: Context, apk: File) {
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.files", apk)
        ctx.startActivity(
            Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }
}
