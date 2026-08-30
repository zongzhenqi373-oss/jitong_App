# im_server：跨平台 IM 服务端（asio Reactor + 线程池并发 + SQLite + protobuf）
# 在 VS Code 中通过 CMake Tools 一键构建/调试；命令行：
#   cmake -S . -B build && cmake --build build -j8 && ./build/im_server
cmake_minimum_required(VERSION 3.20)
project(im_server LANGUAGES CXX)

# 导出每个源文件的真实编译命令，供 VS Code C/C++ IntelliSense 使用。
# 避免编辑器从多个 im.pb.h / Protocol.h 候选中索引到旧构建产物。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IM_SERVER_BUILD_TESTS "Build im_server e2e tests" ON)
option(IM_SERVER_BUILD_AI_SMOKE "Build manual AI API smoke tool" ON)

if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# ---- 依赖：协议库（pb）、SQLite、Threads ----
if(NOT TARGET im_protocol)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol ${CMAKE_BINARY_DIR}/protocol)
endif()
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/sqlite
        /usr/local/opt/sqlite
        /opt/homebrew/opt/openssl@3
        /usr/local/opt/openssl@3
    )
endif()
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
# 添加OpenSSL依赖
find_package(OpenSSL REQUIRED)
find_path(ARGON2_INCLUDE_DIR argon2.h HINTS /opt/homebrew/include /usr/local/include)
find_library(ARGON2_LIBRARY argon2 HINTS /opt/homebrew/lib /usr/local/lib)
if(NOT ARGON2_INCLUDE_DIR OR NOT ARGON2_LIBRARY)
    message(FATAL_ERROR "Argon2 library not found; install libargon2 (macOS: brew install argon2)")
endif()

set(ASIO_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../client_core/third_party/asio/include)
set(HTTPLIB_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../client_core/third_party/httplib)

# ---------------- im_server_lib（业务库，供可执行文件与测试共用） ----------------
add_library(im_server_lib STATIC
    src/db/Database.cpp
    src/db/DbWriteQueue.cpp
    src/session/Presence.cpp
    src/session/Session.cpp
    src/routing/Dispatcher.cpp
    src/routing/ProtocolRouter.cpp
    src/handlers/AuthHandler.cpp
    src/handlers/AiHandler.cpp
    src/handlers/MessageHandler.cpp
    src/handlers/FriendHandler.cpp
    src/handlers/RoamHandler.cpp
    src/handlers/SystemHandler.cpp
    src/media/HttpFileServer.cpp
    src/core/Server.cpp
    src/auth/TokenService.cpp
    src/auth/LoginRateLimiter.cpp
    src/auth/DeviceProof.cpp
    src/crypto/AppCrypto.cpp
    src/ai/AiConfig.cpp
    src/ai/AiContextBuilder.cpp
    src/ai/AiRequestLimiter.cpp
    src/ai/AnthropicModelClient.cpp
    src/ai/OpenAIModelClient.cpp
)
target_include_directories(im_server_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${ASIO_INCLUDE}
            ${HTTPLIB_INCLUDE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
            ${CMAKE_CURRENT_SOURCE_DIR}/../client_core/include
            ${ARGON2_INCLUDE_DIR}
)
target_compile_definitions(im_server_lib PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED CPPHTTPLIB_OPENSSL_SUPPORT)
# 修改链接库
target_link_libraries(
    im_server_lib 
    PUBLIC 
        im_protocol 
        SQLite3::SQLite3 
        Threads::Threads 
        OpenSSL::SSL 
        OpenSSL::Crypto
        ${ARGON2_LIBRARY}
)

if(WIN32)
    target_link_libraries(im_server_lib PUBLIC ws2_32)
endif()
if(APPLE)
    # cpp-httplib 的 SSLClient 使用 macOS Keychain 加载系统可信 CA。
    target_link_libraries(im_server_lib PUBLIC "-framework Security")
endif()

# ---------------- im_server 可执行文件 ----------------
add_executable(im_server src/main.cpp)
target_link_libraries(im_server PRIVATE im_server_lib)
target_compile_definitions(im_server PRIVATE
    IM_SERVER_DEFAULT_CERT="${CMAKE_CURRENT_SOURCE_DIR}/config/dev_server.crt"
    IM_SERVER_DEFAULT_KEY="${CMAKE_CURRENT_SOURCE_DIR}/config/dev_server.key"
    IM_SERVER_DEFAULT_APP_IDENTITY_KEY="${CMAKE_CURRENT_SOURCE_DIR}/config/dev_app_identity_private.pem"
)

# 手动真实 API 冒烟工具，不注册为 CTest，避免自动构建/测试误消耗额度。
if(IM_SERVER_BUILD_AI_SMOKE)
    add_executable(ai_api_smoke tools/ai_api_smoke.cpp)
    target_link_libraries(ai_api_smoke PRIVATE im_server_lib)
endif()

# ---------------- e2e 测试（真服务端 + client_core 真客户端回环） ----------------
if(IM_SERVER_BUILD_TESTS)
    enable_testing()
    find_program(OPENSSL_CLI openssl REQUIRED)
    set(TEST_TLS_DIR ${CMAKE_BINARY_DIR}/config)
    set(TEST_TLS_CERT ${TEST_TLS_DIR}/test_server.crt)
    set(TEST_TLS_KEY ${TEST_TLS_DIR}/test_server.key)
    set(TEST_TLS_CA ${TEST_TLS_DIR}/ca.crt)
    set(TEST_APP_IDENTITY_KEY ${TEST_TLS_DIR}/test_app_identity_private.pem)
    add_custom_target(test_tls_material
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_TLS_DIR}
        COMMAND ${OPENSSL_CLI} req -x509 -newkey rsa:2048 -sha256 -nodes
                -keyout ${TEST_TLS_KEY} -out ${TEST_TLS_CERT} -days 30
                -subj /CN=im.example.com
                -addext subjectAltName=DNS:im.example.com,IP:127.0.0.1
        COMMAND ${CMAKE_COMMAND} -E copy ${TEST_TLS_CERT} ${TEST_TLS_CA}
        COMMAND ${OPENSSL_CLI} genpkey -algorithm ED25519 -out ${TEST_APP_IDENTITY_KEY}
        BYPRODUCTS ${TEST_TLS_CERT} ${TEST_TLS_KEY} ${TEST_TLS_CA} ${TEST_APP_IDENTITY_KEY}
        VERBATIM
    )
    set(CLIENT_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(NOT TARGET client_core)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../client_core ${CMAKE_BINARY_DIR}/client_core)
    endif()

    add_executable(test_e2e tests/test_e2e.cpp)
    add_dependencies(test_e2e test_tls_material)
    target_compile_definitions(test_e2e PRIVATE
        IM_SERVER_TEST_CERT="${TEST_TLS_CERT}"
        IM_SERVER_TEST_KEY="${TEST_TLS_KEY}"
        IM_SERVER_TEST_APP_IDENTITY_KEY="${TEST_APP_IDENTITY_KEY}"
    )
    target_link_libraries(test_e2e PRIVATE im_server_lib client_core)
    target_include_directories(test_e2e PRIVATE ${ASIO_INCLUDE})
    add_test(NAME e2e COMMAND test_e2e)
    set_tests_properties(e2e PROPERTIES DISABLED TRUE)

    add_executable(test_database_concurrency tests/test_database_concurrency.cpp)
    target_link_libraries(test_database_concurrency PRIVATE im_server_lib)
    add_test(NAME database_concurrency COMMAND test_database_concurrency)

    add_executable(test_token_service tests/test_token_service.cpp)
    target_link_libraries(test_token_service PRIVATE im_server_lib)
    add_test(NAME token_service COMMAND test_token_service)

    add_executable(test_login_rate_limiter tests/test_login_rate_limiter.cpp)
    target_link_libraries(test_login_rate_limiter PRIVATE im_server_lib)
    add_test(NAME login_rate_limiter COMMAND test_login_rate_limiter)

    add_executable(test_device_proof tests/test_device_proof.cpp)
    target_link_libraries(test_device_proof PRIVATE im_server_lib)
    add_test(NAME device_proof COMMAND test_device_proof)

    add_executable(test_db_write_queue tests/test_db_write_queue.cpp)
    target_link_libraries(test_db_write_queue PRIVATE im_server_lib)
    add_test(NAME db_write_queue COMMAND test_db_write_queue)

    add_executable(test_conversation_migration tests/test_conversation_migration.cpp)
    target_link_libraries(test_conversation_migration PRIVATE im_server_lib)
    add_test(NAME conversation_migration COMMAND test_conversation_migration)

    add_executable(test_protocol_router tests/test_protocol_router.cpp)
    target_link_libraries(test_protocol_router PRIVATE im_server_lib)
    add_test(NAME protocol_router COMMAND test_protocol_router)

    add_executable(test_ai_model_client tests/test_ai_model_client.cpp)
    target_link_libraries(test_ai_model_client PRIVATE im_server_lib)
    add_test(NAME ai_model_client COMMAND test_ai_model_client)

    add_executable(test_ai_context tests/test_ai_context.cpp)
    target_link_libraries(test_ai_context PRIVATE im_server_lib)
    add_test(NAME ai_context COMMAND test_ai_context)

    add_executable(test_ai_request_limiter tests/test_ai_request_limiter.cpp)
    target_link_libraries(test_ai_request_limiter PRIVATE im_server_lib)
    add_test(NAME ai_request_limiter COMMAND test_ai_request_limiter)

    add_executable(test_app_crypto tests/test_app_crypto.cpp)
    target_link_libraries(test_app_crypto PRIVATE im_server_lib)
    add_test(NAME app_crypto COMMAND test_app_crypto)
endif()
