plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("com.google.protobuf")
    id("com.google.devtools.ksp")
}

android {
    namespace = "com.jitong.im"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.jitong.im"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.5.0" // M4+：默认直连/头像/资料卡/图片收发（+面板）
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }

    // 与 C++ 双端共用同一份 protocol/im.proto（单一协议事实源）
    // Kotlin DSL 没有 protobuf 插件动态注册的 proto 访问器，需显式按扩展类型配置
    sourceSets {
        named("main") {
            // AGP8 的 AndroidSourceSet 静态类型上没有 extensions，需显式转为 ExtensionAware
            (this as org.gradle.api.plugins.ExtensionAware).extensions
                .configure<org.gradle.api.file.SourceDirectorySet>("proto") {
                    srcDir("../../protocol")
                }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

// protoc 现场生成 protobuf-javalite 代码，与 client_core/im_server 的 CMake 策略一致：
// 生成物不进仓库，构建时由本机 protoc 产出，避免版本错配。
protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:3.25.3"
    }
    generateProtoTasks {
        all().forEach { task ->
            task.builtins {
                create("java") {
                    option("lite")
                }
            }
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.09.03"))
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.5")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.5")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("com.google.protobuf:protobuf-javalite:3.25.3")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.coil-kt:coil-compose:2.7.0")
    // Android 系统 Conscrypt 在部分 API 34 镜像中不暴露 Ed25519 KeyFactory。
    // 使用 BC 轻量级 API完成应用层 ServerHello 身份签名验证，不替换系统 TLS Provider。
    implementation("org.bouncycastle:bcprov-jdk18on:1.77")

    // Room：消息/会话本地库（FTS4 独立存储，同事务双写）
    implementation("androidx.room:room-runtime:2.6.1")
    implementation("androidx.room:room-ktx:2.6.1")
    ksp("androidx.room:room-compiler:2.6.1")

    // SQLCipher：给 Room 落盘的 SQLite 文件加密（真实密钥由 DbKeyManager 用登录密码派生的
    // 包装密钥保护，不依赖 Android Keystore，纯标准密码学原语，理论上可移植到其他平台）
    implementation("net.zetetic:sqlcipher-android:4.6.1")
    implementation("androidx.sqlite:sqlite:2.4.0")

    // MMKV：KV 凭证/配置（对齐 QQNT 双存储：MMKV=KV，Room=消息）
    implementation("com.tencent:mmkv-static:1.3.5")

    debugImplementation("androidx.compose.ui:ui-tooling")
    testImplementation("junit:junit:4.13.2")
}
