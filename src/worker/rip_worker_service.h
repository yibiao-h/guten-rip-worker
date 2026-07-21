#pragma once

#include "rip_worker.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <gutenprint/gutenprint.h>

class RipWorkerService final : public gutenrip::worker::RipWorker::Service
{
public:
    RipWorkerService();
    ~RipWorkerService() override;

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
};
