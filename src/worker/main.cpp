#include "rip_worker_service.h"
#include "xml_data_path.h"

#include <grpcpp/grpcpp.h>

#include <grpc/impl/grpc_types.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

void setEnv(const char* name, const std::filesystem::path& value)
{
#ifdef _WIN32
    _wputenv_s(L"STP_DATA_PATH", value.wstring().c_str());
#else
    setenv(name, value.string().c_str(), 1);
#endif
}

void configureGutenprintDataPath(const char* executablePath)
{
    std::string warning;
    const std::filesystem::path executableDir =
        std::filesystem::absolute(executablePath).parent_path();
    const auto dataPath = findRepositoryXmlDataPath(
        executableDir, std::filesystem::current_path(), &warning);
    if (dataPath) {
        setEnv("STP_DATA_PATH", *dataPath);
        std::cout << "STP_DATA_PATH=" << dataPath->string() << '\n';
        return;
    }
    std::cerr << (warning.empty()
                      ? "STP_DATA_PATH is not set and xml was not found near the worker.\n"
                      : warning + "\n");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:50051";
    if (!isLoopbackEndpoint(address)) {
        std::cerr << "Refusing to listen on non-loopback address: " << address << '\n';
        return 1;
    }
    configureGutenprintDataPath(argv[0]);

    RipWorkerService service(address);
    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, kMaxGprcMessageBytes);
    builder.AddChannelArgument(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, kMaxGprcMessageBytes);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "Unable to start guten-rip-worker on " << address << '\n';
        return 1;
    }

    std::cout << "guten-rip-worker listening on " << address << '\n';
    server->Wait();
    return 0;
}
