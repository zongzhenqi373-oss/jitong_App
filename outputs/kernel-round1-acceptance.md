# 第一轮验收报告：P0 + P1 + P2

> 范围：执行规划第 21 节「第一轮建议执行范围」= P0 Android 基线 → P1 Native 构建 → P2 JNI 生命周期和平台接口。
> 执行日期：2026-09-07
> 结论（代码审查修订后）：**第一轮有条件完成，尚未通过最终门禁。**  
> P0：Search Golden 已在设备执行 65/65 通过；Media Golden runner 已建立并准确拦截 AVIF 偏色。P1：桌面端和双 ABI 构建完成，仅 arm64 模拟器实跑；P2：审查修正及 JVM/设备回归已完成。宿主机 ASan+UBSan、Android HWASan、明确 CheckJNI 已完成；x86_64 实跑按本轮决定延期，真机仍待补。

---

## 1. 环境准备（本轮实际安装的依赖）

| 项 | 版本 | 获取方式 |
|---|---|---|
| Android NDK | `27.3.13750724` (r27d) | `sdkmanager --install` |
| Android CMake | `3.22.1` | `sdkmanager --install` |
| JDK（Gradle/AGP 用） | Temurin `17.0.20.1` | 下载到 `~/jdks`（本机原有 8/11/26，AGP 8.5.2 需要 17） |
| Gradle Wrapper | `8.9` | 已生成 `jitong_android/gradlew`（此前仓库缺失） |
| OpenSSL（Android 静态库） | `3.0.15` | `scripts/build-openssl-android.sh`，arm64-v8a + x86_64 |
| protobuf + abseil（Android 静态库） | `35.1`（runtime 报 `7.35.1`） | `scripts/build-protobuf-android.sh`，arm64-v8a + x86_64 |
| 宿主机 protoc | `libprotoc 35.1` | Homebrew（与目标 runtime 同主版本） |

复用命令（换机器/升级依赖时）：

```bash
export JAVA_HOME=/Users/xiaozong/jdks/jdk-17.0.20.1+1/Contents/Home
./scripts/build-openssl-android.sh  arm64-v8a 33
./scripts/build-openssl-android.sh  x86_64    33
./scripts/build-protobuf-android.sh arm64-v8a 33
./scripts/build-protobuf-android.sh x86_64    33
cd jitong_android && ./gradlew :app:assembleDebug
```

---

## 2. P0：Android 行为基线

| 任务 | 状态 | 产物 |
|---|---|---|
| P0-T01 建立基线测试目录 | ✅ | `outputs/kernel-baseline/BASELINE.md`、`client_core/tests/golden/`、`app/src/androidTest/.../golden/` |
| P0-T02 固化登录/Token/防重入/重连 | ✅ | `auth-baseline.md`（含 18 条 Golden 场景 A-01..A-18） |
| P0-T03 固化消息/漫游/幂等 | ✅ | `message-baseline.md`（含 18 条 Golden 场景 M-01..M-18） |
| P0-T04 固化搜索 | ✅ | `search-baseline.md` + `search-golden.json`（**65 条**用例） |
| P0-T05 固化媒体 | ✅ | `media-baseline.md`（含 20 条 Golden 场景 D-01..D-20） |

### 验证

```bash
# 脱敏门禁（手机号 / 疑似 token，公开 pin 与 Ed25519 公钥已白名单）
grep -rnE '1[3-9][0-9]{9}' outputs/kernel-baseline client_core/tests/golden   # 无输出
grep -rnoE '[A-Za-z0-9+/=_-]{32,}' ... | grep -v '/' | grep -vE '...'         # 无输出
python3 -c "import json;print(len(json.load(open('outputs/kernel-baseline/search-golden.json'))['cases']))"
```

结果：`phone: clean`、`token: clean`、`cases: 65`，schema 校验通过（用例 id 唯一、高亮区间均在正文范围内）。

### P0 过程中发现并固化的现状事实（对后续阶段有约束力）

| 编号 | 事实 | 影响 |
|---|---|---|
| D-01 | Android **没有**冷启动自动 Token 登录（不做自动登录，仅回填账号） | V3 §6.2 的 `sdk.start()` 属**新增能力**，P5 需实现 |
| D-02 | HTTP 文件服务 401 **不触发** refresh | P5 `TokenManager` 需补齐 |
| M-01 | **无**消息级超时重发，**无** Outbox 恢复 | P7 需新增 |
| N-02 | **无** seq 缺口补洞（仅靠 `minSeq` 游标） | P8 需新增 |
| N-03 | 消息落库与会话更新是**两个独立事务** | P7 需合并为单事务 |
| P-01 | 上传是**整文件单次 POST**，无分片/续传 | P11 需新增 |
| P-03 | 纵向长图缩略图是**居中裁剪**，非 V3 要求的「顶部首屏」 | **待产品确认**，见 `media-baseline.md` §4 |

---

## 3. P1：打通 Android Native 构建

### P1-T01 CMake 组件化

| 验收点 | 结果 |
|---|---|
| 新增 `CLIENT_CORE_WITH_SQLITE` | ✅ `ON/OFF` 均构建通过 |
| 新增 `CLIENT_CORE_WITH_MEDIA` | ✅ 关闭时跳过 cpp-httplib 与 `uploadMedia/downloadMedia` |
| Android 构建关闭 tests/tools | ✅ 交叉编译时自动 FORCE OFF |
| 宿主机 protoc 与目标 libprotobuf 分离 | ✅ `IM_PROTOC_EXECUTABLE` 可显式指定；交叉编译未指定时 `FATAL_ERROR` |
| 进入共享库的静态库启用 PIC | ✅ `libclient_core.a`、`libim_protocol.a` 均带 `-fPIC` |
| 桌面端原构建路径可用 | ✅ 全开 3/3 测试通过 |

```bash
cmake -S client_core -B build/client-core-desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/client-core-desktop -j8
ctest --test-dir build/client-core-desktop --output-on-failure
# 100% tests passed, 3/3（protocol / storage / integration）

cmake -S client_core -B build/client-core-min -DCLIENT_CORE_WITH_SQLITE=OFF -DCLIENT_CORE_WITH_MEDIA=OFF
ctest --test-dir build/client-core-min --output-on-failure
# 100% tests passed, 2/2（storage 按开关跳过）
```

### P1-T02 Android externalNativeBuild

| 验收点 | 结果 |
|---|---|
| NDK / CMake 配置 | ✅ `ndkVersion 27.3.13750724`、`cmake 3.22.1`、ABI `arm64-v8a` + `x86_64` |
| 接入 OpenSSL Android 目标库 | ✅ 3.0.15 静态库（两 ABI） |
| 接入 protobuf C++ Android 运行库 | ✅ 35.1 + abseil（两 ABI） |
| 首阶段关闭 SQLite / Media | ✅ `CLIENT_CORE_WITH_SQLITE=OFF`、`CLIENT_CORE_WITH_MEDIA=OFF` |
| 两个 ABI 生成 `libjitong_kernel.so` | ✅ |

```bash
cd jitong_android && ./gradlew :app:externalNativeBuildDebug
# BUILD SUCCESSFUL
# app/build/intermediates/cxx/.../obj/arm64-v8a/libjitong_kernel.so   8.4 MB  ARM aarch64
# app/build/intermediates/cxx/.../obj/x86_64/libjitong_kernel.so      8.5 MB  x86-64
```

完整 APK：

```bash
./gradlew clean :app:assembleDebug   # BUILD SUCCESSFUL，87 MB
unzip -l app-debug.apk | grep libjitong_kernel
# lib/arm64-v8a/libjitong_kernel.so   6,180,208
# lib/x86_64/libjitong_kernel.so      6,413,952   (已 strip)
```

### P1-T03 Native Self Test

设备上实跑（`arm64-v8a`，API 34 模拟器）：

```text
kernel=0.5.0        protobuf=7.35.1      openssl=3.0.15
abi=arm64-v8a       sizeof_long=8
frame_codec=ok      endianness=ok        result=ok
```

`frame_codec` 复用 `TcpTransport` 真实的 `encodeLen32/decodeLen32`（含 0/1/4/255/65535/10MB/0xDEADBEEF 七组往返 + 大端逐字节核对），验证的是**链路实际使用的实现**而非复制品。

### P1 过程中解决的关键问题

| 问题 | 根因 | 解决 |
|---|---|---|
| `Could NOT find Protobuf` | NDK toolchain 把 `CMAKE_FIND_ROOT_PATH_MODE_PACKAGE` 设为 `ONLY`，`find_package` 只在 sysroot 搜索，忽略 `CMAKE_PREFIX_PATH` | 依赖前缀同时加入 `CMAKE_FIND_ROOT_PATH`（`app/src/main/cpp/CMakeLists.txt`） |
| protobuf 链接期缺 abseil 符号 | CMake 自带 `FindProtobuf`（module 模式）不带出 `absl::*` | 改用 `find_package(Protobuf CONFIG REQUIRED)`，由 `protobuf-config.cmake` 传递 abseil |
| `no-apps / no-docs` 配置失败 | 这两个选项是 OpenSSL 3.1+ 才有 | 3.0.15 只用 `no-shared no-tests` |
| `nativeCreate` 恒返回 0 | `ClientCore` 构造即创建 `TcpTransport`，而它无条件 `load_verify_file(caFile)`；空路径抛 `No such file or directory` | `caFile` 为空时回退 `set_default_verify_paths()`，**仍保持 `verify_peer`，未做任何验证降级**（`client_core/src/TcpTransport.cpp`） |
| `nativeVersion()` 返回兜底值 | CMake 的 `-DVAR=` 只是 CMake 变量，不会自动成为 C++ 宏 | `target_compile_definitions(JITONG_KERNEL_VERSION="...")` |

---

## 4. P2：JNI 生命周期与平台接口

### P2-T01 NativeSdkHandle

`app/src/main/cpp/native_sdk_handle.{h,cpp}`

- **`jlong` 是不透明 id，不是裸指针**。改用进程内句柄表（`id → shared_ptr<NativeSdkHandle>`）：
  Java 侧 double-destroy 或 use-after-destroy 只会得到「句柄无效」，不会 UAF。
- `acquire()` 在锁内返回 `shared_ptr<im::ClientCore>`，使用期间对象不会被销毁。
- `destroying` 用 CAS 置位，置位后拒绝新调用；`releaseHandle` 幂等、并发安全。
- 析构顺序（审查后修正）：拒绝新 API → `setEventSink(nullptr)` → 停止并销毁 Core → 释放 observer；**析构在锁外执行**，避免持锁析构导致回调重入死锁。

### P2-T02 JniObserver

`app/src/main/cpp/jni_observer.{h,cpp}`

- 缓存 `JavaVM*` + `jclass` + 14 个 `jmethodID`，回调路径不做 `FindClass/GetMethodID`。
- `sink_` 为 `NewGlobalRef`，析构时 `DeleteGlobalRef`。
- **只在本次临时 Attach 时才 Detach**（`GetEnv` 返回 `JNI_EDETACHED` 才 Attach），不会误 Detach Java 线程。
- 每次调用后 `ExceptionCheck` + `ExceptionDescribe` + `ExceptionClear`，异常不跨越 JNI 边界。
- `LocalString` RAII 释放 `jstring` 局部引用，避免 Attach 线程局部引用表溢出。
- 回调只进入可靠 `Channel`，不做业务（Kotlin `NativeEventSink`）。第一版曾使用容量 64 的 `SharedFlow + DROP_OLDEST`，审查发现会丢登录、消息和 ACK，现已移除丢弃策略；P5/P7 前还需按 State/可靠事件/可覆盖进度正式分流。

### P2-T03 平台原子能力接口

`app/src/main/cpp/PlatformServices.h`（C++ 侧）+ `core/platform/*`（Kotlin 侧）

`IPlatformKeystore` / `IPlatformFile` / `IPlatformImage` / `ISecureKv` / `INetworkObserver`，**全部不含业务语义**（无 Token、消息、秒传、缩略图概念；`encodeAvif` 只编码，质量与尺寸由 C++ 决定）。本轮完成的是 C++/Kotlin 两侧接口和 Kotlin 原子能力实现，尚未完成 JNI Proxy 注册，因此不能表述为“内核已经能够调用平台能力”。

Kotlin 实现：`AndroidKeystoreProvider`、`AndroidFileProvider`、`AndroidImagePlatform`、`AndroidSecureKv`、`AndroidNetworkObserver`。

### P2-T04 生命周期压力测试

`NativeLifecycleTest`（8 个用例，跑在 API 34 模拟器）：

```bash
export ANDROID_SERIAL=emulator-5554
./gradlew :app:connectedAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.NativeLifecycleTest
# Finished 8 tests ... BUILD SUCCESSFUL
```

| 用例 | 结果 |
|---|---|
| `nativeVersion_isNotEmpty` | ✅ |
| `selfTest_reportsOk` | ✅ |
| `sdk_startAndStop_leavesNoHandle` | ✅ |
| `doubleDestroy_isIdempotent` | ✅ |
| `staleHandle_isRejected` | ✅ |
| `lifecycleStressTest_100Rounds_passes` | ✅ |
| `eventSink_receivesCallbacksFromNativeThreads` | ✅ |
| `instrumentationContext_isAvailable` | ✅ |

压力测试实测：

```text
rounds=100  callbacks=2056  baseline_handles=0  live_handles=0  leaked=0  failures=0  result=ok
```

覆盖：创建/销毁 100 轮、重复销毁幂等、野句柄被拒、4 个 native 线程持续回调的同时销毁句柄（Attach/Detach 路径被真实覆盖 2000+ 次）。

CheckJNI：已在 `emulator-5554`（API 34 userdebug）显式设置 `debug.checkjni=1`。运行前后读取属性均为 `1`，应用进程日志出现 `com.jitong.im: Late-enabling -Xcheck:jni`；8 项 `NativeLifecycleTest` 全部通过，日志未出现 `JNI DETECTED ERROR`、已删除 GlobalRef、局部引用表溢出或错误 Detach。CheckJNI 与 HWASan 分开运行，避免两个进程级诊断器互相干扰。

---

## 5. 交付物清单

**基线（P0）**

```text
outputs/kernel-baseline/BASELINE.md          环境与脱敏约定
outputs/kernel-baseline/auth-baseline.md     登录/Token/防重入/重连 + 18 条场景
outputs/kernel-baseline/message-baseline.md  消息/漫游/幂等 + 18 条场景
outputs/kernel-baseline/search-baseline.md   搜索与高亮算法
outputs/kernel-baseline/search-golden.json   65 条可重复用例
outputs/kernel-baseline/media-baseline.md    图片/秒传/传输 + 20 条场景
client_core/tests/golden/README.md           内核侧 Golden 测试约定
app/src/androidTest/.../golden/README.md     Android 侧导出/回归约定
```

**Native（P1/P2）**

```text
app/src/main/cpp/CMakeLists.txt              Android 构建入口
app/src/main/cpp/im_core_jni.cpp             JNI 绑定 + 自检 + 生命周期 + 压力测试
app/src/main/cpp/native_sdk_handle.{h,cpp}   句柄表与销毁
app/src/main/cpp/jni_observer.{h,cpp}        事件桥（Attach/Detach/GlobalRef/异常）
app/src/main/cpp/PlatformServices.h          平台原子能力接口
app/src/main/java/com/jitong/im/core/       NativeBindings / NativeEventSink / JitongSdk / KernelBackend
app/src/main/java/com/jitong/im/core/platform/  五个 Android 平台实现
app/src/androidTest/.../NativeLifecycleTest.kt  8 个设备端用例
```

**构建脚本与配置**

```text
scripts/build-openssl-android.sh             可复用，支持 arm64-v8a / x86_64
scripts/build-protobuf-android.sh            自动跟随宿主机 protoc 版本
jitong_android/gradlew + gradle-wrapper.jar  此前缺失，已生成
jitong_android/app/build.gradle.kts          externalNativeBuild / ABI / JDK runner / 测试依赖
jitong_android/.gitignore                    排除 343MB 预编译依赖
client_core/CMakeLists.txt                   组件开关 + PIC
protocol/CMakeLists.txt                      宿主机 protoc 分离 + PIC
client_core/include/client_core/ClientCore.h Media 组件裁剪
client_core/src/{ClientCore,TcpTransport}.cpp 同步裁剪与空 CA 回退
```

---

## 6. 未完成项与风险（**需你确认**）

| 项 | 状态 | 说明 |
|---|---|---|
| **Sanitizer UAF 检查** | ✅ 已执行 | macOS 宿主机 ASan+UBSan：`client_core` 3/3、`im_server` 12/12（含安全 e2e）通过；Android 14 arm64 按 NDK 官方建议使用 HWASan，两个模拟器各 8/8 生命周期测试通过，未报告 tag mismatch/UAF。Android 传统 ASan 已被官方标为不再支持，且与本项目原 `libc++_static + exceptions` 不兼容，因此不把其启动 `SIGILL` 误记为代码缺陷 |
| **x86_64 ABI 实跑** | ⚠️ 仅构建 | 两 ABI 的 `.so` 均已构建并打包，但设备验证只在 `arm64-v8a` 模拟器完成（本机无 x86_64 模拟器） |
| **真机验证** | ❌ 未做 | 仅模拟器 |
| **ADR-001 ~ ADR-010** | ⚠️ 部分 | 本轮实际已决策 ADR-001（protobuf 版本跟随宿主机 protoc 主版本）、ADR-002（OpenSSL 用 3.0.15 LTS 源码交叉编译，脚本化）；其余 ADR 尚未形成文档 |
| **Search Golden** | ✅ arm64 模拟器通过 | `SearchGoldenTest` 在真实 Room FTS4 + TinyPinyin 上执行 65/65 通过；首次执行修正了错误用例 S-055：`yjfs` → `jyfs` |
| **Media Golden** | ⚠️ runner 已完成、门禁失败 | `media-golden.json` + `MediaGoldenTest` 已建立，验证原图/小缩略图/大缩略图的尺寸、AVIF 格式和 RGB 误差；当前白色绿色通道 255→172，正确阻断 |
| **完整业务 e2e** | ⚠️ 前置能力未完成 | 实测 TLS 1.3 成功后，旧 C++ ClientCore 直接发 Login(1000)，服务端因等待 AppClientHello(1036) 正确断开；新增 `security_e2e` 验证该 fail-close，普通与 ASan 下均通过。登录/消息/HTTP 文件完整 e2e 必须等待 P4 C++ ClientSecureChannel，不能绕过安全校验 |
| **P0 门禁「当前 Android Debug 构建通过」** | ✅ 补验 | 报告开始时无法验证（缺 gradlew/JDK17），本轮已补齐并通过 |

### 6.1 第一轮代码审查与修正记录

本节记录静态代码审查发现的问题。标记“已修正”只表示代码已经修改，仍需重新执行构建和设备测试后才能关闭验收项。

| 编号 | 级别 | 审查发现 | 修正内容 | 当前状态 |
|---|---|---|---|---|
| R1 | P0 | `NativeSdkHandle` 先析构 `JniObserver`，`ClientCore` 仍持有非拥有裸指针；Core/Transport 关闭回调可能 UAF | 销毁前先 `setEventSink(nullptr)`，随后 `disconnect`、销毁 Core，最后释放 observer | 100 轮销毁/并发回调通过，宿主机 ASan 与 Android HWASan 均未报告 UAF |
| R2 | P0 | `AndroidFileProvider` 只返回 `pfd.fd`，未保留 PFD 所有权；`close()` 为空；`content://` 无法执行 `readRange` | 使用并发句柄表保留 `ParcelFileDescriptor`，`readRange` 使用 `Os.pread`，`close` 确定释放 | 本地文件和真实 `content://` 随机读、关闭测试通过；长期 fd 泄漏压力仍待补 |
| R3 | P1 | 所有事件使用 `SharedFlow(64)+DROP_OLDEST`，突发时会静默丢登录、消息、ACK | 第一轮改为 `Channel.UNLIMITED` 可靠入队；P5/P7 前按状态、可靠事件和可覆盖进度拆流 | 1000 条突发事件无丢失测试通过 |
| R4 | P1 | `NewStringUTF` 接受 Modified UTF-8，直接传标准 UTF-8 的 emoji 存在异常/乱码风险 | JNI 增加严格 UTF-8 → UTF-16 转换并使用 `NewString` | 真实 C++ 回调 `你好🙂𐐷` 往返通过；非法 UTF-8 负向测试待补 |
| R5 | P1 | `JitongSdk.start()` 挂载 sink 失败后仍保留 Native handle | 失败分支立即 `nativeDestroy`，只在挂载成功后发布 handle | 代码已修正，待失败注入测试 |
| R6 | P1 | `KernelBackendSelector.initialize()` 可重复覆盖，与“进程内不可热切换”不一致 | 增加一次初始化保护，后续调用忽略 | 代码已修正，待单元测试 |
| R7 | P1 | 运行中替换 observer 会造成 Core 裸事件指针的悬空窗口 | 一个 handle 只允许绑定一次 observer，重复绑定失败 | 代码已修正，待并发测试 |
| R8 | P2 | `copyTo + delete` 被错误描述为原子重命名；`openWrite` 未截断旧文件 | 只接受同文件系统 `ATOMIC_MOVE`；不支持时明确失败；写入增加 `MODE_TRUNCATE` | 截断旧文件、原子替换测试通过；异常掉电测试待补 |
| R9 | P2 | 网络回调可重复注册且没有注销，存在 Context/Callback 泄漏 | 增加单次注册保护、线程可见性和 `close/unregisterNetworkCallback` | 代码已修正，待生命周期测试 |
| R10 | P2 | `Bitmap.copyPixelsToBuffer` 被当作稳定 RGBA8，实际原始内存通道顺序不应作为跨平台契约 | 改为显式 ARGB Int 与 RGBA 字节逐通道转换 | **设备测试失败**：白色回读为 `255,172,255`；问题已缩小到 avif-coder 2.2.1 编码/兜底解码链路，不能关闭 |
| R11 | P1 | `JniObserver` 即使部分 `GetMethodID` 失败也会被视为挂载成功 | 增加完整 method ID/GlobalRef 有效性检查，失败时拒绝绑定 | 代码已修正，待签名错误注入测试 |

### 6.2 修正后的门禁状态

| 阶段 | 修正后判定 | 进入下一阶段前必须完成 |
|---|---|---|
| P0 | 条件通过 | Search Golden 已关闭；修复 AVIF 偏色并让 Media Golden 通过 |
| P1 | 条件通过 | x86_64 实跑 Self Test；至少一次真机 Self Test；补依赖源码 SHA-256 校验 |
| P2 | 条件通过 | AVIF 颜色问题；非法 UTF-8/observer 签名失败等剩余负向测试；真机与 x86_64 实跑 |

因此，当前不应直接把第二轮标记为可执行。应先完成一个 **Round 1.1 收口批次**：

```text
重新编译与静态检查
→ Native 生命周期和事件压力回归
→ 真实 Core 回调与销毁并发测试
→ content URI / fd 所有权测试
→ 中文、emoji 和非法 UTF-8 JNI 测试
→ 纯红/绿/蓝/透明 RGBA 通道测试
→ ASan/HWASan 与明确 CheckJNI 取证（已完成）
→ x86_64 和真机最小运行验证
→ 回填 Search/Media Golden
```

本次审查修正后的实际验证（2026-09-07）：

```bash
cd jitong_android
JAVA_HOME=/Users/xiaozong/jdks/jdk-17.0.20.1+1/Contents/Home \
  ./gradlew :app:testDebugUnitTest :app:compileDebugKotlin :app:externalNativeBuildDebug
# BUILD SUCCESSFUL
# arm64-v8a / x86_64 Native 均完成构建

cmake --build build/client-core-desktop -j8
ctest --test-dir build/client-core-desktop --output-on-failure
# 3/3 passed（protocol / storage / integration）

ANDROID_SERIAL=emulator-5554 ./gradlew :app:connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.NativeLifecycleTest,com.jitong.im.core.Round11ClosureTest
# 14 项：13 passed，1 failed
# 失败：imagePlatform_rgbaContractPreservesPrimaryChannels
# 观测：白色期望 RGB 三通道 >180，实际为 255,172,255
```

设备测试已覆盖生命周期、并发回调、1000 条可靠事件、标准 UTF-8 非 BMP 字符、文件句柄、`content://` 随机读、截断与原子替换。AVIF 测试暴露了真实颜色问题，因此本轮总体仍不能无条件关闭。

Sanitizer 与 CheckJNI 补验：

```bash
# 宿主机 C/C++：实际执行 14 项，不把 disabled e2e 的 0 tests 算作通过
bash scripts/native_sanitizer_test.sh
# client_core 3/3；im_server 11/11；无 ASan/UBSan 报告

# Android 14 arm64：NDK 27+ 推荐 HWASan；诊断构建切换为 c++_shared
./gradlew clean :app:connectedDebugAndroidTest \
  -Pjitong.hwasan.enabled=true \
  -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.NativeLifecycleTest
# emulator-5554 8/8；emulator-5556 8/8；libjitong_kernel.so NEEDED libclang_rt.hwasan
# 日志无 HWAddressSanitizer tag-mismatch / use-after-free

# CheckJNI 独立运行
adb -s emulator-5554 shell getprop debug.checkjni  # 1
# logcat: com.jitong.im: Late-enabling -Xcheck:jni
# NativeLifecycleTest 8/8，且无 JNI DETECTED ERROR
```

本机两台 AVD 都是 `arm64-v8a`，所以第二台运行增加了环境重复验证，但**不能冒充 x86_64 ABI 实跑**；按本轮决定，x86_64 运行验证延期。Search Golden runner 已实现并 65/65 通过；Media Golden runner 已实现并稳定暴露 AVIF 偏色。完整业务 e2e 的实跑证明其缺口是 C++ ClientSecureChannel，而非测试环境；当前新增的 `security_e2e` 已在普通和 ASan 构建下通过。

### 6.3 自检覆盖范围说明

当前 `nativeSelfTest()` 只验证：

- 4 字节包长的编码/解码往返；
- 大端字节布局；
- Native ABI、OpenSSL 和 protobuf 版本可读取。

尚未验证：

- 非法包长被拒绝；
- Header/Body 分段读取；
- 4 字节小端协议号；
- protobuf payload 边界；
- TLS 握手；
- 真实 Socket 收发。

因此验收措辞统一为“包长端序编码自检通过”，完整消息边界由 P3 测试关闭。

### 6.4 第三方依赖可复现性

OpenSSL/protobuf 构建脚本目前固定了默认版本和下载地址，但尚未校验源码压缩包 SHA-256。补齐前存在上游资源被替换、下载损坏而构建仍继续的供应链风险。P1 最终关闭前应：

1. 在脚本中保存每个允许版本的 SHA-256；
2. 下载后先校验再解压；
3. CI 或内部制品记录 NDK、CMake、编译参数及源码摘要；
4. 依赖升级通过独立 ADR 和双 ABI 回归，不自动跟随宿主机最新版本。

---

## 7. 遗留设计问题（留给 P3/P4 决策）

1. **`TcpTransport` 构造即建 SSL context**：与 P3-T01「每次 connect 创建新的 socket/TLS stream」冲突，P3 重构时应把 SSL context 创建推迟到 `connect()`。
2. **CA 与 Pinning**：当前空 `caFile` 回退系统信任库。P4 必须改为随包分发 CA + SPKI Pinning（Android 侧现用 `sha256/co0kqh…`，已记录在 `auth-baseline.md` §8）。
3. **`resetToLogin()` 不清 `pendingRefreshRequestId`**：已记为现状行为 D-04，P5 需确认是否有意。
4. **JNI DTO 选型（ADR-005）**：本轮事件回调只走标量 + 字符串；`onRoamMessages` 只传批次元信息（正文由内核落库后经 Query 上抛）。P7 引入列表传递时再决定 protobuf envelope 还是手写 DTO。
