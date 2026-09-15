cmake_minimum_required(VERSION 3.20)
project(client_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CLIENT_CORE_BUILD_TESTS "Build client_core unit tests" ON)
option(CLIENT_CORE_BUILD_TOOLS "Build im_cli demo client" ON)

# 组件开关：Android 首阶段（P1）只接入 transport + protocol，关闭 SQLite 与 Media，
# 避免过早引入 SQLCipher/ssl 之外的链接依赖；桌面端默认全开，保持原有构建路径。
option(CLIENT_CORE_WITH_SQLITE "Build the SQLite-backed IStorage implementation" ON)
option(CLIENT_CORE_WITH_MEDIA  "Build HTTP media upload/download (cpp-httplib + OpenSSL)" ON)
# P6-T01：C++ SQLCipher 数据底座（Android 链接 Room 自带 AAR 的 libsqlcipher.so；
# 桌面编译 amalgamation 以跑单测）。与上面的 WITH_SQLITE（明文 Legacy）完全分离。
option(CLIENT_CORE_WITH_SQLCIPHER "Build the SQLCipher-backed native database (P6)" OFF)

# 交叉编译（Android NDK / iOS）时构建机通常无法运行目标产物，默认关闭可执行类目标。
if(ANDROID OR CMAKE_CROSSCOMPILING)
    set(CLIENT_CORE_BUILD_TESTS OFF CACHE BOOL "Build client_core unit tests" FORCE)
    set(CLIENT_CORE_BUILD_TOOLS OFF CACHE BOOL "Build im_cli demo client" FORCE)
endif()

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
set(CLIENT_CORE_SOURCES
    src/ClientCore.cpp
    src/TcpTransport.cpp
    src/transport/FrameCodec.cpp
    src/transport/ClientSecureChannel.cpp
    src/transport/DeviceProof.cpp
    src/account/AccountTypes.cpp
    src/account/AuthStateMachine.cpp
    src/account/TokenManager.cpp
    src/account/DeviceProofService.cpp
    src/account/ReconnectPolicy.cpp
    src/account/AuthRequestBuilder.cpp
    src/account/AccountSession.cpp
    src/account/ClientCoreAuthTransport.cpp
)
if(CLIENT_CORE_WITH_SQLITE)
    list(APPEND CLIENT_CORE_SOURCES src/SqliteStorage.cpp)
endif()

add_library(client_core STATIC ${CLIENT_CORE_SOURCES})

# 进入 .so 的静态库必须启用 PIC（Android 由 app/src/main/cpp/CMakeLists.txt 链接成共享库）
set_target_properties(client_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(client_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

# 组件宏为 PUBLIC：public 头 ClientCore.h 用同一组宏裁剪声明，保证声明与实现一致
target_compile_definitions(client_core
    PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED
    PUBLIC  $<$<BOOL:${CLIENT_CORE_WITH_SQLITE}>:CLIENT_CORE_WITH_SQLITE=1>
            $<$<BOOL:${CLIENT_CORE_WITH_MEDIA}>:CLIENT_CORE_WITH_MEDIA=1>
)

if(CLIENT_CORE_WITH_MEDIA)
    target_include_directories(client_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)
    target_compile_definitions(client_core PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

find_package(Threads REQUIRED)
target_link_libraries(client_core PUBLIC Threads::Threads im_protocol)

# asio standalone 在 Windows 需要 ws2_32
if(WIN32)
    target_link_libraries(client_core PUBLIC ws2_32)
endif()

# OpenSSL：TLS 传输层必需，Media 也复用同一份
if(APPLE AND NOT ANDROID)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/openssl@3
        /usr/local/opt/openssl@3
    )
endif()
find_package(OpenSSL REQUIRED)
target_link_libraries(client_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# SQLite：可选组件
if(CLIENT_CORE_WITH_SQLITE)
    if(APPLE AND NOT ANDROID)
        list(APPEND CMAKE_PREFIX_PATH
            /opt/homebrew/opt/sqlite
            /usr/local/opt/sqlite
        )
    endif()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(client_core PUBLIC SQLite3::SQLite3)
endif()

# ---------------- P6-T01：SQLCipher（C++ 数据底座） ----------------
# 与 CLIENT_CORE_WITH_SQLITE（明文系统 SQLite，仅供桌面 Legacy/兼容测试）完全分离。
#
# Android 走「方案 A」：直接链接 Room 自带 AAR 内的 libsqlcipher.so（4.6.1，与
# net.zetetic:sqlcipher-android 完全一致）→ 零体积增量、无「进程内双 SQLCipher」符号冲突。
# 依据见 outputs/kernel-round5-encryption-note.md（S0 决策记录）。
#
# 桌面：编译 SQLCipher amalgamation（由 scripts/build-sqlcipher-host.sh 生成），
#       以便在本机跑 CipherDatabase / Schema / 迁移等 C++ 单测；编译特性与 Android 对齐。
#       头文件 sqlite3.h 已入库，sqlite3.c 由脚本生成（见 .gitignore）。
if(CLIENT_CORE_WITH_SQLCIPHER)
    if(ANDROID)
        set(SQLCIPHER_AAR "" CACHE FILEPATH
            "sqlcipher-android AAR 路径（默认从 Gradle 缓存自动探测）")
        if(NOT SQLCIPHER_AAR)
            file(GLOB _sqlcipher_aar_candidates
                "$ENV{HOME}/.gradle/caches/modules-2/files-2.1/net.zetetic/sqlcipher-android/*/*/sqlcipher-android-*.aar")
            if(_sqlcipher_aar_candidates)
                list(GET _sqlcipher_aar_candidates -1 SQLCIPHER_AAR)
            endif()
        endif()
        if(NOT EXISTS "${SQLCIPHER_AAR}")
            message(FATAL_ERROR
                "CLIENT_CORE_WITH_SQLCIPHER=ON 需要 sqlcipher-android AAR，但未能自动找到。\n"
                "请显式传入：-DSQLCIPHER_AAR=<path/to/sqlcipher-android-4.6.1.aar>")
        endif()
        set(_sqlcipher_extract "${CMAKE_CURRENT_BINARY_DIR}/sqlcipher_aar")
        file(MAKE_DIRECTORY "${_sqlcipher_extract}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf "${SQLCIPHER_AAR}"
            WORKING_DIRECTORY "${_sqlcipher_extract}"
            RESULT_VARIABLE _sqlcipher_extract_rv
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _sqlcipher_extract_rv EQUAL 0)
            message(FATAL_ERROR "解压失败：${SQLCIPHER_AAR}")
        endif()
        set(_sqlcipher_so "${_sqlcipher_extract}/jni/${ANDROID_ABI}/libsqlcipher.so")
        if(NOT EXISTS "${_sqlcipher_so}")
            message(FATAL_ERROR "AAR 内缺少 jni/${ANDROID_ABI}/libsqlcipher.so：${SQLCIPHER_AAR}")
        endif()
        # 运行时由 AGP 从 AAR 打包进 APK；链接器只记录 SONAME=libsqlcipher.so
        add_library(sqlcipher SHARED IMPORTED GLOBAL)
        set_target_properties(sqlcipher PROPERTIES IMPORTED_LOCATION "${_sqlcipher_so}")
        target_include_directories(client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlcipher)
        message(STATUS "SQLCipher(Android/方案A): ${_sqlcipher_so}")
    else()
        set(_sqlcipher_amalg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlcipher")
        if(NOT EXISTS "${_sqlcipher_amalg}/sqlite3.c")
            message(FATAL_ERROR
                "缺少 SQLCipher amalgamation：${_sqlcipher_amalg}/sqlite3.c\n"
                "请先运行：bash scripts/build-sqlcipher-host.sh")
        endif()
        # amalgamation 是 C 源文件；本工程 project() 只声明了 CXX，这里启用 C
        enable_language(C)
        add_library(sqlcipher STATIC "${_sqlcipher_amalg}/sqlite3.c")
        set_target_properties(sqlcipher PROPERTIES C_STANDARD 99)
        target_include_directories(sqlcipher PUBLIC "${_sqlcipher_amalg}")
        # 编译特性与 Android 侧 AAR 对齐（见 encryption-note.md 第 3.1 节实测 compile_options）
        target_compile_definitions(sqlcipher PUBLIC
            SQLITE_HAS_CODEC=1
            SQLCIPHER_CRYPTO_OPENSSL
            SQLITE_THREADSAFE=1
            SQLITE_TEMP_STORE=2
            SQLITE_ENABLE_FTS3
            SQLITE_ENABLE_FTS4
            SQLITE_ENABLE_FTS5
            SQLITE_DEFAULT_PAGE_SIZE=4096
        )
        find_package(Threads REQUIRED)
        target_link_libraries(sqlcipher PUBLIC OpenSSL::Crypto OpenSSL::SSL
                                              Threads::Threads ${CMAKE_DL_LIBS})
        set_target_properties(sqlcipher PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    target_link_libraries(client_core PUBLIC sqlcipher)
    # SQLITE_HAS_CODEC 使 sqlite3.h 中的 sqlite3_key/rekey 声明可见
    target_compile_definitions(client_core PUBLIC
        $<$<BOOL:${CLIENT_CORE_WITH_SQLCIPHER}>:CLIENT_CORE_WITH_SQLCIPHER=1>
        SQLITE_HAS_CODEC=1)
    # P6 数据底座实现（T01 打开器；T02 路径/密钥桥；T03 Schema/迁移）
    target_sources(client_core PRIVATE
        src/storage/CipherDatabase.cpp
        src/storage/DatabasePaths.cpp
        src/storage/DbKeyBridge.cpp
        src/storage/SchemaManager.cpp
        src/storage/DbCommandQueue.cpp
        src/storage/ReadPool.cpp
        src/storage/MigrationImporter.cpp
        src/storage/NativeDatabase.cpp
        src/storage/NativeRepository.cpp
        src/runtime/ClientRuntime.cpp
        src/message/MessageService.cpp
        src/search/SearchService.cpp
        src/friend/FriendService.cpp
        src/ai/AiSuggestionService.cpp
        src/sync/SyncService.cpp)

    # 把 migrations/*.sql 生成为内嵌字符串头：Android/桌面都不依赖运行时文件
    set(_sql_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/migrations")
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(READ "${_sql_dir}/001_initial.sql" _sql_001)
    file(READ "${_sql_dir}/002_fts_identity.sql" _sql_002)
    file(READ "${_sql_dir}/003_p7_state_machines.sql" _sql_003)
    file(READ "${_sql_dir}/004_local_sequence.sql" _sql_004)
    file(READ "${_sql_dir}/005_friend_domain.sql" _sql_005)
    file(READ "${_sql_dir}/006_ai_suggestions.sql" _sql_006)
    file(WRITE "${_gen_dir}/migrations_sql.h"
"// 自动生成，勿手改（来源：src/storage/migrations/*.sql）
#ifndef CLIENT_CORE_GENERATED_MIGRATIONS_SQL_H
#define CLIENT_CORE_GENERATED_MIGRATIONS_SQL_H
namespace im { namespace storage {
inline const char* kMigration001Sql = R\"SQL(
${_sql_001}
)SQL\";
inline const char* kMigration002Sql = R\"SQL(
${_sql_002}
)SQL\";
inline const char* kMigration003Sql = R\"SQL(
${_sql_003}
)SQL\";
inline const char* kMigration004Sql = R\"SQL(
${_sql_004}
)SQL\";
inline const char* kMigration005Sql = R\"SQL(
${_sql_005}
)SQL\";
inline const char* kMigration006Sql = R\"SQL(
${_sql_006}
)SQL\";
} } // namespace
#endif
")
    target_include_directories(client_core PRIVATE "${_gen_dir}")
endif()

# cpp-httplib（httplib::SSLClient）在 macOS 上用系统信任库校验证书，需要显式链接这两个 framework
if(APPLE)
    target_link_libraries(client_core PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

# ---------------- 单元测试 ----------------
if(CLIENT_CORE_BUILD_TESTS)
    enable_testing()

    # 测试用 assert() 做断言：Release 构建默认带 NDEBUG，会让断言变成空操作，
    # 表现为"测试瞬间通过"却什么都没验证。这里显式取消该宏。
    add_compile_options(-UNDEBUG)

    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE client_core)
    target_include_directories(test_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    add_test(NAME protocol COMMAND test_protocol)

    # FrameCodec：线格式唯一实现，边界矩阵见 tests/test_frame_codec.cpp
    add_executable(test_frame_codec tests/test_frame_codec.cpp)
    target_link_libraries(test_frame_codec PRIVATE client_core)
    target_include_directories(test_frame_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME frame_codec COMMAND test_frame_codec)

    # P7-G1：Harness Fake 基础设施自检（确定性时钟/调度/随机 + 故障注入）
    add_executable(test_testing_fakes tests/test_testing_fakes.cpp)
    target_link_libraries(test_testing_fakes PRIVATE client_core)
    add_test(NAME testing_fakes COMMAND test_testing_fakes)

    # P7-G4：同步水位与缺洞状态机（乱序/缺洞/重复收敛、退避预算、不形成风暴）
    add_executable(test_sync_tracker tests/test_sync_tracker.cpp)
    target_link_libraries(test_sync_tracker PRIVATE client_core)
    add_test(NAME sync_tracker COMMAND test_sync_tracker)
    add_executable(test_recovery_resolver tests/test_recovery_resolver.cpp)
    target_link_libraries(test_recovery_resolver PRIVATE client_core)
    add_test(NAME recovery_resolver COMMAND test_recovery_resolver)

    # P7-G1：稳定 DTO 与结构化 diff（Legacy/Native 逐字段比较，输出首个字段差异）
    add_executable(test_golden_diff tests/test_golden_diff.cpp)
    target_link_libraries(test_golden_diff PRIVATE client_core)
    add_test(NAME golden_diff COMMAND test_golden_diff)

    # P7-G1：协议 fuzz corpus（截断/尾随/未知字段/错误长度/超大计数/确定性随机）
    add_executable(test_protocol_fuzz tests/test_protocol_fuzz.cpp)
    target_link_libraries(test_protocol_fuzz PRIVATE client_core)
    target_include_directories(test_protocol_fuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    add_test(NAME protocol_fuzz COMMAND test_protocol_fuzz)

    # P7-G1：事件契约（dbVersion 单调失效通知 + 有界完成登记表 exactly-once）
    add_executable(test_event_contract tests/test_event_contract.cpp)
    target_link_libraries(test_event_contract PRIVATE client_core)
    add_test(NAME event_contract COMMAND test_event_contract)

    # P7-G1：Golden runner（JSON 唯一真相源 + 逐字段 diff + 首个差异定位）
    add_executable(test_golden_runner tests/test_golden_runner.cpp)
    target_link_libraries(test_golden_runner PRIVATE client_core)
    target_compile_definitions(test_golden_runner PRIVATE
        CLIENT_CORE_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/app-security-v1.json")
    add_test(NAME golden_runner COMMAND test_golden_runner)

    if(CLIENT_CORE_WITH_SQLITE)
        add_executable(test_storage tests/test_storage.cpp)
        target_link_libraries(test_storage PRIVATE client_core)
        add_test(NAME storage COMMAND test_storage)
    endif()

    # 回环集成测试：自带 asio TLS 假服务端（跟真服务端一样强制握手），需要 asio 头文件 + 一份自签测试证书
    find_program(OPENSSL_CLI openssl REQUIRED)
    set(TEST_TLS_DIR ${CMAKE_BINARY_DIR}/config)
    set(TEST_TLS_CERT ${TEST_TLS_DIR}/test_server.crt)
    set(TEST_TLS_KEY ${TEST_TLS_DIR}/test_server.key)
    add_custom_command(
        OUTPUT ${TEST_TLS_CERT} ${TEST_TLS_KEY}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_TLS_DIR}
        COMMAND ${OPENSSL_CLI} req -x509 -newkey rsa:2048 -sha256 -nodes
                -keyout ${TEST_TLS_KEY} -out ${TEST_TLS_CERT} -days 365
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

    # Transport：真实 loopback TLS 上的消息边界、生命周期与重连 50 次
    add_executable(test_transport tests/test_transport.cpp)
    add_dependencies(test_transport client_core_test_tls_material)

    # R2-F02：多级证书链 pinning 用例的材料。
    # 生成 root CA（自签）→ 由 root 签发的 leaf；服务端发送 chain=leaf+root，
    # 客户端信任 root、并 pin **leaf** 的 SPKI。这样验证回调会对 depth0(leaf) 与
    # depth1(root) 都触发，正是修复前会误拒的最小链场景。
    set(TEST_CHAIN_ROOT_KEY ${TEST_TLS_DIR}/test_chain_root.key)
    set(TEST_CHAIN_ROOT_CRT ${TEST_TLS_DIR}/test_chain_root.crt)
    set(TEST_CHAIN_LEAF_KEY ${TEST_TLS_DIR}/test_chain_leaf.key)
    set(TEST_CHAIN_LEAF_CRT ${TEST_TLS_DIR}/test_chain_leaf.crt)
    set(TEST_CHAIN_LEAF_CSR ${TEST_TLS_DIR}/test_chain_leaf.csr)
    set(TEST_CHAIN_FULL_CRT ${TEST_TLS_DIR}/test_chain_fullchain.crt)
    set(TEST_CHAIN_EXT ${TEST_TLS_DIR}/test_chain_leaf.ext)
    add_custom_command(
        OUTPUT ${TEST_CHAIN_FULL_CRT} ${TEST_CHAIN_LEAF_KEY} ${TEST_CHAIN_ROOT_CRT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_TLS_DIR}
        # root CA
        COMMAND ${OPENSSL_CLI} req -x509 -newkey rsa:2048 -sha256 -nodes
                -keyout ${TEST_CHAIN_ROOT_KEY} -out ${TEST_CHAIN_ROOT_CRT} -days 365
                -subj /CN=Jitong-Test-Root-CA
        # leaf key + CSR
        COMMAND ${OPENSSL_CLI} req -newkey rsa:2048 -sha256 -nodes
                -keyout ${TEST_CHAIN_LEAF_KEY} -out ${TEST_CHAIN_LEAF_CSR}
                -subj /CN=im.example.com
        # leaf SAN 扩展
        COMMAND ${CMAKE_COMMAND} -E echo "subjectAltName=DNS:im.example.com,DNS:localhost,IP:127.0.0.1" > ${TEST_CHAIN_EXT}
        # 用 root 签发 leaf
        COMMAND ${OPENSSL_CLI} x509 -req -in ${TEST_CHAIN_LEAF_CSR}
                -CA ${TEST_CHAIN_ROOT_CRT} -CAkey ${TEST_CHAIN_ROOT_KEY}
                -CAcreateserial -sha256 -days 365
                -extfile ${TEST_CHAIN_EXT} -out ${TEST_CHAIN_LEAF_CRT}
        # fullchain = leaf + root（服务端发送整条链）
        COMMAND ${CMAKE_COMMAND} -E cat ${TEST_CHAIN_LEAF_CRT} ${TEST_CHAIN_ROOT_CRT} > ${TEST_CHAIN_FULL_CRT}
        VERBATIM
    )
    add_custom_target(client_core_test_chain_material
        DEPENDS ${TEST_CHAIN_FULL_CRT} ${TEST_CHAIN_LEAF_KEY} ${TEST_CHAIN_ROOT_CRT})
    add_dependencies(test_transport client_core_test_chain_material)

    target_link_libraries(test_transport PRIVATE client_core)
    target_include_directories(test_transport PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include)
    target_compile_definitions(test_transport PRIVATE
        ASIO_STANDALONE ASIO_NO_DEPRECATED
        CLIENT_CORE_TEST_CERT="${TEST_TLS_CERT}"
        CLIENT_CORE_TEST_KEY="${TEST_TLS_KEY}"
        CLIENT_CORE_TEST_CHAIN_FULLCHAIN="${TEST_CHAIN_FULL_CRT}"
        CLIENT_CORE_TEST_CHAIN_LEAF_KEY="${TEST_CHAIN_LEAF_KEY}"
        CLIENT_CORE_TEST_CHAIN_LEAF_CRT="${TEST_CHAIN_LEAF_CRT}"
        CLIENT_CORE_TEST_CHAIN_ROOT_CRT="${TEST_CHAIN_ROOT_CRT}")
    if(WIN32)
        target_link_libraries(test_transport PRIVATE ws2_32)
    endif()
    add_test(NAME transport COMMAND test_transport)
    # 网络相关测试必须有上界超时：挂起要变成失败，而不是无限等待
    set_tests_properties(transport PROPERTIES TIMEOUT 180)

    # 安全通道：四步握手、加密帧与负向用例
    add_executable(test_secure_channel tests/test_secure_channel.cpp)
    target_link_libraries(test_secure_channel PRIVATE client_core)
    target_include_directories(test_secure_channel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # R2-F04：Golden 向量以 JSON 文件为唯一真相源，测试从文件读取期望值再与生产实现
    # 比对（而不是把期望硬编码进 .cpp）。三端（C++/Android/Server）共享同一份 JSON。
    target_compile_definitions(test_secure_channel PRIVATE
        CLIENT_CORE_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/app-security-v1.json")
    add_test(NAME secure_channel COMMAND test_secure_channel)

    # ---------------- P5 认证域（纯 C++，无 IO，确定性单测） ----------------
    add_executable(test_auth_state_machine tests/test_auth_state_machine.cpp)
    target_link_libraries(test_auth_state_machine PRIVATE client_core)
    add_test(NAME auth_state_machine COMMAND test_auth_state_machine)

    add_executable(test_token_manager tests/test_token_manager.cpp)
    target_link_libraries(test_token_manager PRIVATE client_core)
    add_test(NAME token_manager COMMAND test_token_manager)

    add_executable(test_device_proof_service tests/test_device_proof_service.cpp)
    target_link_libraries(test_device_proof_service PRIVATE client_core)
    target_include_directories(test_device_proof_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME device_proof_service COMMAND test_device_proof_service)

    add_executable(test_reconnect_policy tests/test_reconnect_policy.cpp)
    target_link_libraries(test_reconnect_policy PRIVATE client_core)
    add_test(NAME reconnect_policy COMMAND test_reconnect_policy)

    add_executable(test_auth_request_builder tests/test_auth_request_builder.cpp)
    target_link_libraries(test_auth_request_builder PRIVATE client_core)
    target_include_directories(test_auth_request_builder PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    add_test(NAME auth_request_builder COMMAND test_auth_request_builder)

    add_executable(test_account_session tests/test_account_session.cpp)
    target_link_libraries(test_account_session PRIVATE client_core)
    target_include_directories(test_account_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME account_session COMMAND test_account_session)

    # P6-T01：SQLCipher 数据库打开器（仅在开启 SQLCIPHER 时构建）
    if(CLIENT_CORE_WITH_SQLCIPHER)
        add_executable(test_cipher_database tests/test_cipher_database.cpp)
        target_link_libraries(test_cipher_database PRIVATE client_core)
        add_test(NAME cipher_database COMMAND test_cipher_database)

        add_executable(test_database_paths tests/test_database_paths.cpp)
        target_link_libraries(test_database_paths PRIVATE client_core)
        add_test(NAME database_paths COMMAND test_database_paths)

        add_executable(test_schema_migrations tests/test_schema_migrations.cpp)
        target_link_libraries(test_schema_migrations PRIVATE client_core)
        add_test(NAME schema_migrations COMMAND test_schema_migrations)

        # P7-G2：Schema v3 状态机实体迁移（0/1/2→3、幂等、新表语义、索引命中）
        add_executable(test_schema_v3 tests/test_schema_v3.cpp)
        target_link_libraries(test_schema_v3 PRIVATE client_core)
        add_test(NAME schema_v3 COMMAND test_schema_v3)

        # P7-G2：Repository 业务级事务（消息/FTS/会话/未读同事务、幂等、outbox 生命周期）
        add_executable(test_native_repository tests/test_native_repository.cpp)
        target_link_libraries(test_native_repository PRIVATE client_core)
        add_test(NAME native_repository COMMAND test_native_repository)

        # P7-G3：ClientRuntime 生命周期与隔离（可重入、并发 destroy 风暴、generation 隔离）
        add_executable(test_client_runtime tests/test_client_runtime.cpp)
        target_compile_definitions(test_client_runtime PRIVATE CLIENT_CORE_TEST_HOOKS=1)
        target_link_libraries(test_client_runtime PRIVATE client_core)
        add_test(NAME client_runtime COMMAND test_client_runtime)
        add_executable(test_search_service tests/test_search_service.cpp)
        target_link_libraries(test_search_service PRIVATE client_core)
        add_test(NAME search_service COMMAND test_search_service)
        set_tests_properties(client_runtime PROPERTIES TIMEOUT 30)

        # P7-G4：MessageService 编排（C++ 发号、Outbox/Inbox 合并入口、Ack 合并）
        add_executable(test_message_service tests/test_message_service.cpp)
        target_link_libraries(test_message_service PRIVATE client_core)
        add_test(NAME message_service COMMAND test_message_service)

        # P7-G4：FriendService 编排（好友资料/申请幂等、同意/拒绝、删除、Presence 分离）
        add_executable(test_friend_service tests/test_friend_service.cpp)
        target_link_libraries(test_friend_service PRIVATE client_core)
        add_test(NAME friend_service COMMAND test_friend_service)

        # P7-G4：AiSuggestionService 状态机（single-flight/超时/取消/迟到抑制）
        add_executable(test_ai_suggestion tests/test_ai_suggestion.cpp)
        target_link_libraries(test_ai_suggestion PRIVATE client_core)
        add_test(NAME ai_suggestion COMMAND test_ai_suggestion)

        # P7-G4：SyncService 编排（水位/缺洞/退避 + sync_gaps 影子落库）
        add_executable(test_sync_service tests/test_sync_service.cpp)
        target_link_libraries(test_sync_service PRIVATE client_core)
        add_test(NAME sync_service COMMAND test_sync_service)

        add_executable(test_db_command_queue tests/test_db_command_queue.cpp)
        target_link_libraries(test_db_command_queue PRIVATE client_core)
        add_test(NAME db_command_queue COMMAND test_db_command_queue)

        add_executable(test_read_pool tests/test_read_pool.cpp)
        target_link_libraries(test_read_pool PRIVATE client_core)
        add_test(NAME read_pool COMMAND test_read_pool)

        add_executable(test_room_import tests/test_room_import.cpp)
        target_link_libraries(test_room_import PRIVATE client_core)
        add_test(NAME room_import COMMAND test_room_import)

        # P6-T05：跨进程排他锁（第二进程打开被拒）
        add_executable(test_process_lock tests/test_process_lock.cpp)
        target_link_libraries(test_process_lock PRIVATE client_core)
        add_test(NAME process_lock COMMAND test_process_lock)

        # P6-T05：迁移开关三态 + 清理影子库
        add_executable(test_migration_switch tests/test_migration_switch.cpp)
        target_link_libraries(test_migration_switch PRIVATE client_core)
        add_test(NAME migration_switch COMMAND test_migration_switch)

        # P6-T05：强杀恢复（fork + _exit 模拟非优雅退出，验证 WAL 崩溃恢复）
        add_executable(test_kill_recovery tests/test_kill_recovery.cpp)
        target_link_libraries(test_kill_recovery PRIVATE client_core)
        add_test(NAME kill_recovery COMMAND test_kill_recovery)

        # P6-T05：NativeDatabase 生命周期门禁（F03 超时迟到回调 / F09 close 并发）
        add_executable(test_native_database_lifecycle tests/test_native_database_lifecycle.cpp)
        target_link_libraries(test_native_database_lifecycle PRIVATE client_core)
        add_test(NAME native_database_lifecycle COMMAND test_native_database_lifecycle)
    endif()
endif()

# ---------------- im_cli 命令行演示客户端 ----------------
# im_cli 演示图片/文件上传下载，依赖 Media 组件；Media 关闭时一并跳过
if(CLIENT_CORE_BUILD_TOOLS AND CLIENT_CORE_WITH_MEDIA)
    add_executable(im_cli tools/im_cli.cpp)
    target_link_libraries(im_cli PRIVATE client_core)
    target_include_directories(im_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    # 默认信任 im_server 自带的开发证书；本机路径由 CMake 注入，不再硬编码某个人电脑上的绝对路径
    target_compile_definitions(im_cli PRIVATE
        IM_CLI_DEFAULT_CA="${CMAKE_CURRENT_SOURCE_DIR}/../im_server/config/dev_server.crt"
    )
endif()
