#pragma once

#include "rip_worker.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <gutenprint/gutenprint.h>

#include <string>

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
