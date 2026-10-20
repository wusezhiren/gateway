package com.homenet.espgw.data

import android.content.Context
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** ESP 固件里的 esp_app_desc_t 信息 */
data class FwImage(val project: String, val version: String, val size: Long)

object Firmware {
    /** App 内置的网关固件（发版时由 Gradle 从 gateway/build 拷入） */
    const val GATEWAY_ASSET = "firmware/gateway.bin"
    const val GATEWAY_PROJECT = "esp_s3_gateway"

    /**
     * 解析固件头：24 字节镜像头 + 8 字节段头之后是 esp_app_desc_t，
     * magic 0xABCD5432，version 在 +16（32 字节），project_name 在 +48（32 字节）。
     */
    fun parseHeader(head: ByteArray, size: Long): FwImage? {
        if (head.size < 32 + 80 || head[0] != 0xE9.toByte()) return null
        val bb = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
        if (bb.getInt(32) != 0xABCD5432.toInt()) return null
        fun str(off: Int) = String(head, off, 32, Charsets.US_ASCII).substringBefore('\u0000')
        return FwImage(project = str(32 + 48), version = str(32 + 16), size = size)
    }

    fun openBundledGateway(ctx: Context): InputStream = ctx.assets.open(GATEWAY_ASSET)

    /** 读内置网关固件的版本；没有内置或不是网关固件返回 null */
    fun bundledGateway(ctx: Context): FwImage? = runCatching {
        openBundledGateway(ctx).use { input ->
            val head = ByteArray(256)
            var n = 0
            while (n < head.size) {
                val r = input.read(head, n, head.size - n)
                if (r < 0) break
                n += r
            }
            var size = n.toLong()
            val buf = ByteArray(16384)
            while (true) {
                val r = input.read(buf)
                if (r < 0) break
                size += r
            }
            parseHeader(head, size)?.takeIf { it.project == GATEWAY_PROJECT }
        }
    }.getOrNull()

    /** 比较 x.y.z 版本号，>0 表示 a 更新 */
    fun compareVersions(a: String, b: String): Int {
        val pa = a.split(".", "-").map { it.toIntOrNull() ?: 0 }
        val pb = b.split(".", "-").map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(pa.size, pb.size)) {
            val d = pa.getOrElse(i) { 0 } - pb.getOrElse(i) { 0 }
            if (d != 0) return d
        }
        return 0
    }
}
