// im_server 入口
// 用法: im_server [port] [dbPath] [certPath] [keyPath] [httpPort]
//   默认 port=24563, dbPath=data/im.db（上传目录 uploads/），httpPort=port+1（文件服务）
#include <cstdlib>
#include <iostream>
#include "Server.h"

int main(int argc, char* argv[])
{
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 24563;
    const std::string dbPath = argc > 2 ? argv[2] : "data/im.db";
    const std::string certPath = argc > 3 ? argv[3] : IM_SERVER_DEFAULT_CERT;
    const std::string keyPath = argc > 4 ? argv[4] : IM_SERVER_DEFAULT_KEY;
    const std::uint16_t httpPort = argc > 5 ? static_cast<std::uint16_t>(std::atoi(argv[5])) : static_cast<std::uint16_t>(port + 1);

    imsrv::Server server(port, /*ioThreads=*/4, /*dbWorkers=*/2, dbPath, "uploads", certPath, keyPath, httpPort);
    if (!server.start()) {
        std::cerr << "启动失败" << std::endl;
        return 1;
    }
    server.run();  // 阻塞直到 SIGINT/SIGTERM
    server.stop(); // 在主线程执行关停（join 全部工作线程）
    return 0;
}
