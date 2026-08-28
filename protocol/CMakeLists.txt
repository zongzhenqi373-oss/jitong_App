# im_protocol：由 im.proto 生成的协议静态库
# 被 client_core 与 im_server 共同链接，保证全工程只有一份 pb 生成物（避免符号冲突）。
# Windows 的 VS/qmake 不使用本 CMake，它们直接编译仓库提交的 protocol/generated/im.pb.*。
cmake_minimum_required(VERSION 3.20)
project(im_protocol LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CONFIG 模式：新版 protobuf(>=22) 依赖 abseil，其 cmake config 会通过 find_dependency 传递
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/protobuf /usr/local/opt/protobuf
        /opt/homebrew/opt/abseil   /usr/local/opt/abseil)
endif()
find_package(Protobuf CONFIG REQUIRED)

# 构建时用本机 protoc 现场生成，保证生成物与本机运行时主版本严格一致
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/im.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
    OUTPUT  ${PROTO_GEN_DIR}/im.pb.cc ${PROTO_GEN_DIR}/im.pb.h
    COMMAND protobuf::protoc --cpp_out=${PROTO_GEN_DIR}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR} im.proto
    DEPENDS ${PROTO_FILE}
    COMMENT "protoc 生成 im.pb.*（本机运行时版本）"
)

add_library(im_protocol STATIC ${PROTO_GEN_DIR}/im.pb.cc)
target_include_directories(im_protocol PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(im_protocol PUBLIC protobuf::libprotobuf)
