#pragma once

#include "rip_worker.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <gutenprint/gutenprint.h>

#include <cstdint>
#include <string>

// 流式协议资源上限（客户端须使用一致的值）。
inline constexpr std::int64_t kMaxWorkerPixels = 1LL << 30;      // 总像素
inline constexpr std::int64_t kMaxRowBytes = 64LL << 20;         // 单行字节
inline constexpr std::int32_t kMaxGprcMessageBytes = 64 << 20;   // gRPC 消息上限

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
    std::string m_listenEndpoint;
};
