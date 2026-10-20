import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// 版本号：-PappVersion=0.2.0（发版脚本传入），versionCode 由版本号推出
val appVersion = (findProperty("appVersion") as String?) ?: "0.1.2"
val appVersionCode = appVersion.split(".").map { it.toInt() }.let { it[0] * 10000 + it[1] * 100 + it[2] }

// 发布签名放在仓库外的 local/（不上传）；没有时退回调试签名
val keystoreProps = Properties().apply {
    val f = rootProject.file("../local/keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "com.homenet.espgw"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.homenet.espgw"
        // WifiNetworkSpecifier（App 内连网关热点）需要 Android 10
        minSdk = 29
        targetSdk = 35
        versionCode = appVersionCode
        versionName = appVersion
    }

    signingConfigs {
        if (!keystoreProps.isEmpty) {
            create("release") {
                storeFile = file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        val release = signingConfigs.findByName("release")
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = release
        }
        debug {
            // 调试包也用发布签名，两者可以互相覆盖安装
            if (release != null) signingConfig = release
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        // 发版时不跑 lint（要下载很大的依赖，且没有 CI）
        checkReleaseBuilds = false
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/fwassets"))
}

// 把编译好的网关固件（已签名）打进 App：assets/firmware/gateway.bin
val copyGatewayFirmware by tasks.registering(Copy::class) {
    from(rootProject.file("../gateway/build/esp_s3_gateway.bin"))
    into(layout.buildDirectory.dir("generated/fwassets/firmware"))
    rename { "gateway.bin" }
}
tasks.named("preBuild") { dependsOn(copyGatewayFirmware) }

kotlin {
    jvmToolchain(17)
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    // 云端 MQTT（EMQX Cloud，TLS）
    implementation("org.eclipse.paho:org.eclipse.paho.client.mqttv3:1.2.5")
}
