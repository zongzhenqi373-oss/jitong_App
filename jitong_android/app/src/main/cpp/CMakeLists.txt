# Android 侧 Native 构建入口：产出 libjitong_kernel.so
#
# 由 AGP externalNativeBuild 调用，已注入的变量包括：
#   ANDROID_ABI / ANDROID_PLATFORM / CMAKE_ANDROID_ARCH_ABI / ANDROID_NDK
#
# 首阶段（P1）目标：
#   1) 打通 NDK + OpenSSL + protobuf 的交叉编译；
#   2) 产出可被 System.loadLibrary("jitong_kernel") 加载的 .so；
#   3) 提供 nativeVersion() / nativeSelfTest() 自检。
#
# 首阶段关闭 CLIENT_CORE_WITH_SQLITE 与 CLIENT_CORE_WITH_MEDIA（见 P1-T02）。

cmake_minimum_required(VERSION 3.22.1)
project(jitong_kernel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 第三方依赖查找路径
#
# Android 没有系统包管理器，所有原生依赖必须以「预编译静态库 + 头文件」的形式
# 放在 third_party/android/<abi>/ 下，由 scripts/ 下的构建脚本产出。约定布局：
#
#   jitong_android/app/src/main/cpp/third_party/android/<abi>/
#     ├── include/                 # OpenSSL / protobuf 头文件
#     └── lib/
#         ├── libssl.a libcrypto.a
#         └── libprotobuf.a  (或 libprotobuf-lite.a)
#
# CMAKE_PREFIX_PATH 让 find_package(OpenSSL) / find_package(Protobuf) 命中目标机库。
# ---------------------------------------------------------------------------
set(JT_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/android/${ANDROID_ABI}")
if(NOT EXISTS "${JT_THIRD_PARTY}")
    message(FATAL_ERROR
        "缺少 ${ANDROID_ABI} 的预编译依赖：${JT_THIRD_PARTY}\n"
        "请先执行：./scripts/build-openssl-android.sh ${ANDROID_ABI}\n"
        "          ./scripts/build-protobuf-android.sh ${ANDROID_ABI}")
endif()

# Android NDK toolchain 会把 CMAKE_FIND_ROOT_PATH_MODE_PACKAGE 设为 ONLY，
# 导致 find_package 只在 NDK sysroot 中搜索，忽略 CMAKE_PREFIX_PATH。
# 因此除了加 CMAKE_PREFIX_PATH，还必须把依赖前缀加入 CMAKE_FIND_ROOT_PATH。
list(APPEND CMAKE_PREFIX_PATH "${JT_THIRD_PARTY}")
list(APPEND CMAKE_FIND_ROOT_PATH "${JT_THIRD_PARTY}")
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE PATH "Prefix path for find_package" FORCE)
set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}" CACHE PATH "Root path for find_*" FORCE)

message(STATUS "jitong_kernel: ANDROID_ABI = ${ANDROID_ABI}")
message(STATUS "jitong_kernel: JT_THIRD_PARTY = ${JT_THIRD_PARTY}")
message(STATUS "jitong_kernel: CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")

# 宿主机 protoc：交叉编译必须用宿主机可执行文件生成 .pb.cc，
# 不能复用目标机的 protobuf（目标机只有库没有可执行文件）。
# 由 build.gradle.kts 的 -DIM_PROTOC_EXECUTABLE= 传入；缺省时回落到 PATH 中的 protoc。
if(NOT IM_PROTOC_EXECUTABLE)
    find_program(IM_PROTOC_EXECUTABLE NAMES protoc)
endif()
if(NOT IM_PROTOC_EXECUTABLE)
    message(FATAL_ERROR
        "找不到宿主机 protoc。请安装 protobuf 并在 Gradle 中传入："
        "arguments \"-DIM_PROTOC_EXECUTABLE=<path>\"")
endif()
message(STATUS "jitong_kernel: host protoc = ${IM_PROTOC_EXECUTABLE}")

# protobuf 目标机运行库。
# 必须用 CONFIG 模式：新版 protobuf(>=22) 依赖 abseil，只有通过 protobuf-config.cmake
# 才能把 absl::* 传递进 protobuf::libprotobuf 的链接接口；
# 用 CMake 自带的 FindProtobuf(module 模式) 会漏掉 abseil，链接期报 undefined symbol。
set(Protobuf_USE_STATIC_LIBS ON)
find_package(Protobuf CONFIG REQUIRED)
message(STATUS "jitong_kernel: protobuf = ${Protobuf_VERSION} (${Protobuf_LIBRARIES})")

find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# 协议库 + 内核静态库：复用仓库根目录的同一份 CMake，保证桌面/Android 同源
# ---------------------------------------------------------------------------
set(CLIENT_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLIENT_CORE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CLIENT_CORE_WITH_SQLITE OFF CACHE BOOL "" FORCE)   # P1 首阶段关闭，P6 再开
set(CLIENT_CORE_WITH_MEDIA  OFF CACHE BOOL "" FORCE)   # P1 首阶段关闭，P10 再开

# CMAKE_CURRENT_SOURCE_DIR = <repo>/jitong_android/app/src/main/cpp
# 五级 .. 到达仓库根，复用同一份 client_core（桌面/Android 同源）
get_filename_component(JT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
if(NOT EXISTS "${JT_REPO_ROOT}/client_core/CMakeLists.txt")
    message(FATAL_ERROR "未找到 client_core：${JT_REPO_ROOT}/client_core/CMakeLists.txt")
endif()
add_subdirectory(
    "${JT_REPO_ROOT}/client_core"
    "${CMAKE_BINARY_DIR}/client_core"
)
target_compile_definitions(client_core PRIVATE CLIENT_CORE_TEST_HOOKS)

# ---------------------------------------------------------------------------
# JNI 绑定 + 内核共享库
# ---------------------------------------------------------------------------
add_library(jitong_kernel SHARED
    im_core_jni.cpp
    native_sdk_handle.cpp
    jni_observer.cpp
)

# 版本注入：CMake 的 -DVAR=... 只是「CMake 变量」，不会自动成为 C++ 宏，
# 必须显式转成 compile definition，否则 nativeVersion() 会回落到兜底值。
if(JITONG_KERNEL_VERSION)
    message(STATUS "jitong_kernel: version = ${JITONG_KERNEL_VERSION}")
    target_compile_definitions(jitong_kernel PRIVATE
        JITONG_KERNEL_VERSION="${JITONG_KERNEL_VERSION}")
endif()

target_include_directories(jitong_kernel
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # 自检直接复用传输层的帧编解码（encodeLen32/decodeLen32），
        # 保证 SelfTest 校验的就是真实链路使用的实现，而不是复制品。
        # TcpTransport.h 是 client_core 内部头（位于 src/），它又依赖 standalone asio，
        # 因此这里补齐这条内部依赖链。
        ${JT_REPO_ROOT}/client_core/src
        ${JT_REPO_ROOT}/client_core/third_party/asio/include
)

target_compile_definitions(jitong_kernel
    PRIVATE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        CLIENT_CORE_TEST_HOOKS
)

target_link_libraries(jitong_kernel
    PRIVATE
        client_core
        im_protocol
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Android 日志库
find_library(log-lib log)
if(log-lib)
    target_link_libraries(jitong_kernel PRIVATE ${log-lib})
endif()

# 裁剪：隐藏未使用的符号，减小 .so 体积并避免符号冲突
target_link_options(jitong_kernel PRIVATE
    "-Wl,--exclude-libs,ALL"
    "-Wl,--gc-sections"
)
