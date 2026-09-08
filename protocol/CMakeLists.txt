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

# 构建时用本机 protoc 现场生成，保证生成物与本机运行时主版本严格一致。
#
# 交叉编译（Android NDK）时必须分离：
#   - protoc 是「宿主机」可执行程序（生成 .pb.cc/.pb.h）
#   - libprotobuf 是「目标机」运行库（链接进 .so）
# find_package 找到的是目标机 protobuf，其 protoc 目标可能不存在或不可执行。
# 因此允许显式指定宿主机 protoc：
#   cmake ... -DIM_PROTOC_EXECUTABLE=$(which protoc)
if(IM_PROTOC_EXECUTABLE)
    set(IM_PROTOC_COMMAND "${IM_PROTOC_EXECUTABLE}")
    message(STATUS "im_protocol: 使用指定的宿主机 protoc = ${IM_PROTOC_EXECUTABLE}")
elseif(ANDROID OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "交叉编译必须传入宿主机 protoc："
        "-DIM_PROTOC_EXECUTABLE=<path-to-host-protoc>")
else()
    set(IM_PROTOC_COMMAND protobuf::protoc)
endif()

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/im.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
    OUTPUT  ${PROTO_GEN_DIR}/im.pb.cc ${PROTO_GEN_DIR}/im.pb.h
    COMMAND ${IM_PROTOC_COMMAND} --cpp_out=${PROTO_GEN_DIR}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR} im.proto
    DEPENDS ${PROTO_FILE}
    COMMENT "protoc 生成 im.pb.*（宿主机 protoc）"
)

add_library(im_protocol STATIC ${PROTO_GEN_DIR}/im.pb.cc)
# 进入 .so 的静态库必须启用 PIC
set_target_properties(im_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(im_protocol PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(im_protocol PUBLIC protobuf::libprotobuf)
