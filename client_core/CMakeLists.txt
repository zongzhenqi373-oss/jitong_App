cmake_minimum_required(VERSION 3.20)
project(client_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CLIENT_CORE_BUILD_TESTS "Build client_core unit tests" ON)
option(CLIENT_CORE_BUILD_TOOLS "Build im_cli demo client" ON)

# MSVC 默认按系统代码页解析源文件，强制 UTF-8 以正确编译中文注释/字符串
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# pb 生成物策略：统一由 protocol/ 的 im_protocol 静态库提供（构建时本机 protoc 现场生成）。
# Windows 的 VS/qmake 不使用 CMake，它们直接编译仓库提交的 protocol/generated/im.pb.*。
if(NOT TARGET im_protocol)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol ${CMAKE_BINARY_DIR}/protocol)
endif()

# ---------------- client_core 静态库 ----------------
add_library(client_core STATIC
    src/ClientCore.cpp
    src/TcpTransport.cpp
    src/SqliteStorage.cpp
)

target_include_directories(client_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_compile_definitions(client_core PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED CPPHTTPLIB_OPENSSL_SUPPORT)

find_package(Threads REQUIRED)
target_link_libraries(client_core PUBLIC Threads::Threads im_protocol)

# asio standalone 在 Windows 需要 ws2_32
if(WIN32)
    target_link_libraries(client_core PUBLIC ws2_32)
endif()

# SQLite：优先系统/Homebrew 安装
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/sqlite
        /usr/local/opt/sqlite
        /opt/homebrew/opt/openssl@3
        /usr/local/opt/openssl@3
    )
endif()
find_package(SQLite3 REQUIRED)
#添加openssl链接库
find_package(OpenSSL REQUIRED)
target_link_libraries(
    client_core
    PUBLIC
        Threads::Threads
        im_protocol
        SQLite3::SQLite3
        OpenSSL::SSL
        OpenSSL::Crypto
)

# cpp-httplib（httplib::SSLClient）在 macOS 上用系统信任库校验证书，需要显式链接这两个 framework
if(APPLE)
    target_link_libraries(client_core PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

# ---------------- 单元测试 ----------------
if(CLIENT_CORE_BUILD_TESTS)
    enable_testing()

    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE client_core)
    target_include_directories(test_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    add_test(NAME protocol COMMAND test_protocol)

    add_executable(test_storage tests/test_storage.cpp)
    target_link_libraries(test_storage PRIVATE client_core)
    add_test(NAME storage COMMAND test_storage)

    # 回环集成测试：自带 asio TLS 假服务端（跟真服务端一样强制握手），需要 asio 头文件 + 一份自签测试证书
    find_program(OPENSSL_CLI openssl REQUIRED)
    set(TEST_TLS_DIR ${CMAKE_BINARY_DIR}/config)
    set(TEST_TLS_CERT ${TEST_TLS_DIR}/test_server.crt)
    set(TEST_TLS_KEY ${TEST_TLS_DIR}/test_server.key)
    add_custom_command(
        OUTPUT ${TEST_TLS_CERT} ${TEST_TLS_KEY}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_TLS_DIR}
        COMMAND ${OPENSSL_CLI} req -x509 -newkey rsa:2048 -sha256 -nodes
                -keyout ${TEST_TLS_KEY} -out ${TEST_TLS_CERT} -days 2
                -subj /CN=im.example.com
                -addext subjectAltName=DNS:im.example.com,DNS:localhost,IP:127.0.0.1
        VERBATIM
    )
    add_custom_target(client_core_test_tls_material DEPENDS ${TEST_TLS_CERT} ${TEST_TLS_KEY})

    add_executable(test_integration tests/test_integration.cpp)
    add_dependencies(test_integration client_core_test_tls_material)
    target_link_libraries(test_integration PRIVATE client_core)
    target_include_directories(test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include)
    target_compile_definitions(test_integration PRIVATE
        ASIO_STANDALONE ASIO_NO_DEPRECATED
        CLIENT_CORE_TEST_CERT="${TEST_TLS_CERT}"
        CLIENT_CORE_TEST_KEY="${TEST_TLS_KEY}"
    )
    if(WIN32)
        target_link_libraries(test_integration PRIVATE ws2_32)
    endif()
    add_test(NAME integration COMMAND test_integration)
endif()

# ---------------- im_cli 命令行演示客户端 ----------------
if(CLIENT_CORE_BUILD_TOOLS)
    add_executable(im_cli tools/im_cli.cpp)
    target_link_libraries(im_cli PRIVATE client_core)
    target_include_directories(im_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    # 默认信任 im_server 自带的开发证书；本机路径由 CMake 注入，不再硬编码某个人电脑上的绝对路径
    target_compile_definitions(im_cli PRIVATE
        IM_CLI_DEFAULT_CA="${CMAKE_CURRENT_SOURCE_DIR}/../im_server/config/dev_server.crt"
    )
endif()
