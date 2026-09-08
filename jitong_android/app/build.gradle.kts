plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("com.google.protobuf")
    id("com.google.devtools.ksp")
}

// ---------------------------------------------------------------------------
// Native 内核（QQNT 式厚内核）构建配置
//
// 开关：-Pjitong.native.enabled=false 可完全关闭 Native 构建，
// 保证没有 NDK / 预编译依赖的机器仍能正常构建 App（灰度期可秒回退到 Legacy）。
// ---------------------------------------------------------------------------
val nativeEnabled: Boolean =
    (project.findProperty("jitong.native.enabled") as String?)?.toBoolean() ?: true
val hwasanEnabled: Boolean =
    (project.findProperty("jitong.hwasan.enabled") as String?)?.toBoolean() ?: false

/**
 * 宿主机 protoc：交叉编译时必须用宿主机可执行文件生成 im.pb.cc，
 * 不能复用 Android 目标机的 protobuf（那边只有库，没有可执行程序）。
 */
fun findHostProtoc(): String {
    System.getenv("IM_PROTOC_EXECUTABLE")?.takeIf { it.isNotBlank() }?.let { return it }
    listOf("/opt/homebrew/bin/protoc", "/usr/local/bin/protoc", "/usr/bin/protoc")
        .firstOrNull { file(it).exists() }?.let { return it }
    return "protoc" // 交给 PATH，由 CMake 的 find_program 兜底
}

android {
    namespace = "com.jitong.im"
    // avif-coder 的 AAR 以 API 36 编译；只提高编译 API，不改变 minSdk/targetSdk 行为。
    compileSdk = 36

    if (nativeEnabled) {
        ndkVersion = "27.3.13750724"
        externalNativeBuild {
            cmake {
                path = file("src/main/cpp/CMakeLists.txt")
                version = "3.22.1"
            }
        }
    }

    defaultConfig {
        applicationId = "com.jitong.im"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.5.0" // M4+：默认直连/头像/资料卡/图片收发（+面板）

        // 默认 runner 是 JUnit3 的 InstrumentationTestRunner，无法识别 @RunWith(AndroidJUnit4)。
        // Native 内核的生命周期/自检测试必须跑在 AndroidJUnitRunner 上。
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        if (nativeEnabled) {
            ndk {
                // 真机 arm64 + 模拟器 x86_64；不编 32 位
                abiFilters += if (hwasanEnabled) listOf("arm64-v8a")
                else listOf("arm64-v8a", "x86_64")
            }
            externalNativeBuild {
                cmake {
                    // C++17 与桌面端一致；exceptions 保留以便 JNI 层 catch 后转换
                    cppFlags += listOf("-std=c++17", "-fexceptions")
                    arguments += listOf(
                        "-DANDROID_STL=${if (hwasanEnabled) "c++_shared" else "c++_static"}",
                        "-DCMAKE_BUILD_TYPE=Release",
                        // 宿主机 protoc（交叉编译必需）
                        "-DIM_PROTOC_EXECUTABLE=${findHostProtoc()}",
                        // 内核版本注入 .so，用于灰度期校验 Java/Native 版本匹配
                        "-DJITONG_KERNEL_VERSION=${versionName}",
                    )
                    if (hwasanEnabled) arguments += "-DANDROID_SANITIZE=hwaddress"
                }
            }
        }
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
        named("androidTest") {
            // Golden JSON 保持在仓库级 outputs 中，Android/Native 共用同一事实源。
            assets.srcDir("../../outputs/kernel-baseline")
        }
        if (hwasanEnabled) {
            named("debug") {
                resources.srcDir("src/hwasan/resources")
            }
        }
    }

    if (hwasanEnabled) {
        // Android 14 的 HWASan 通过 wrap.sh 在独立进程启动时启用。
        packaging.jniLibs.useLegacyPackaging = true
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        // avif-coder 2.2.x 由 Kotlin 2.3 发布，但这里只使用稳定的 Java 可见 API。
        // 允许 Kotlin 2.0 编译器读取其元数据；运行时统一使用项目的 2.0.20 stdlib。
        freeCompilerArgs.add("-Xskip-metadata-version-check")
    }
}

configurations.configureEach {
    resolutionStrategy.force(
        "org.jetbrains.kotlin:kotlin-stdlib:2.0.20",
        "org.jetbrains.kotlin:kotlin-stdlib-jdk7:2.0.20",
        "org.jetbrains.kotlin:kotlin-stdlib-jdk8:2.0.20",
    )
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

    // 中文消息搜索：入库时同时生成全拼和首字母（例如“今天天气” ->
    // jintiantianqi / jttq），与正文一起写入 FTS4。
    // 原 com.github.promeg 坐标发布在已停止服务的 JCenter，mavenCentral() 无法解析。
    // 使用 Maven Central 上保持相同 com.github.promeg.pinyinhelper API 的再发布版本。
    implementation("me.majiajie:tinypinyin:2.0.3")

    // Android Bitmap 只有 AVIF 解码能力，没有 Bitmap.CompressFormat.AVIF；
    // 使用 libavif/AOM JNI 编码器生成真正的 AVIF 字节。
    implementation("io.github.awxkee:avif-coder:2.2.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
    testImplementation("junit:junit:4.13.2")

    // Instrumented 测试：Native 内核自检与生命周期压力测试必须在真实 ART 上运行
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test:rules:1.6.1")
}
