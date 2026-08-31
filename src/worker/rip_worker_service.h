#pragma once

#include "rip_worker.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <gutenprint/gutenprint.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

// 流式协议资源上限（客户端须使用一致的值）。
inline constexpr std::int64_t kMaxRowBytes = 64LL << 20;         // 单行字节
inline constexpr std::int32_t kMaxGprcMessageBytes = 64 << 20;   // gRPC 消息上限

namespace gutenrip::worker::detail {

bool rawRowRequestMatchesImage(int row,
                               int width,
                               int height,
                               int channels,
                               std::size_t byteLimit);

} // namespace gutenrip::worker::detail

// 仅允许 loopback 监听/连接（host 为 127.0.0.1 / localhost / ::1）。
bool isLoopbackEndpoint(const std::string& endpoint);

class RipWorkerService : public gutenrip::worker::RipWorker::Service
{
public:
    explicit RipWorkerService(std::string listenEndpoint = "127.0.0.1:50051");
    ~RipWorkerService() override;

    void setListenEndpoint(const std::string& endpoint);

    grpc::Status GetCapabilities(grpc::ServerContext* context,
                                 const gutenrip::worker::Empty* request,
                                 gutenrip::worker::Capabilities* response) override;

    grpc::Status GetPrintDriverCatalog(grpc::ServerContext* context,
                                       const gutenrip::worker::PrintDriverCatalogRequest* request,
                                       gutenrip::worker::PrintDriverCatalog* response) override;

    grpc::Status RenderPlaneStream(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<gutenrip::worker::RenderPlaneStreamResponse,
                                 gutenrip::worker::RenderPlaneStreamRequest>* stream) override;

    grpc::Status WorkerHello(grpc::ServerContext* context,
                             const gutenrip::worker::WorkerHelloRequest* request,
                             gutenrip::worker::WorkerHelloResponse* response) override;

private:
    // 同步服务每次只处理一个 RPC，但 gRPC 线程池并发；
    // 整体串行化覆盖 print engine 全局 model 缓存/refcache 竞态。
    std::mutex m_rpcMutex;
    std::string m_listenEndpoint;
};
