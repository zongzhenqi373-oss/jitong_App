# 即通 App

本仓库包含可运行的 Android IM 客户端、C++ 服务端、通用 C++ 客户端核心、共享 Protobuf 协议及相关测试。

## 目录

- `jitong_android/`：Kotlin + Jetpack Compose Android 客户端及单元测试
- `im_server/`：C++17 + Asio + TLS + SQLite IM 服务端及测试
- `client_core/`：C++ 通用客户端核心、CLI 和集成测试
- `protocol/`：C++ / Android 共享的 `im.proto`
- `scripts/`：TLS 开发证书和 Sanitizer/安全测试脚本

## 首次运行

1. 在仓库根目录执行 `bash scripts/generate_dev_tls.sh`。
2. 用 Android Studio 打开 `jitong_android/`并重新构建 App。
3. 安装 CMake、Protobuf、OpenSSL、SQLite3 和 Argon2，然后构建 `im_server/`。
4. AI 功能可按 `im_server/config/ai.env.example` 创建本地 `ai.env`；真实 API Key 不得提交。

开发证书私钥、AI 密钥、本地数据库、上传文件、构建产物和审查报告均已排除。
