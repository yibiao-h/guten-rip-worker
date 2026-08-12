#include "rip_worker_service.h"

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

bool envIsSet(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0';
}

void setEnv(const char* name, const std::filesystem::path& value)
{
#ifdef _WIN32
    _putenv_s(name, value.string().c_str());
#else
    setenv(name, value.string().c_str(), 0);
#endif
}

bool xmlDataPathContainsLocalDrivers(const std::filesystem::path& dataPath)
{
    const std::filesystem::path escp2 = dataPath / "printers" / "escp2.xml";
    std::error_code ec;
    return std::filesystem::is_regular_file(escp2, ec) && !ec &&
           std::filesystem::file_size(escp2, ec) > 0 && !ec;
}

void configureGutenprintDataPath(const char* executablePath)
{
    if (envIsSet("STP_DATA_PATH") &&
        xmlDataPathContainsLocalDrivers(std::getenv("STP_DATA_PATH"))) {
        return;
    }

    const std::filesystem::path executableDir = std::filesystem::absolute(executablePath).parent_path();
    const std::filesystem::path currentDir = std::filesystem::current_path();
    std::vector<std::filesystem::path> candidatesVec;
    candidatesVec.push_back(currentDir.parent_path() / "xml");
    candidatesVec.push_back(executableDir.parent_path() / "xml");
    candidatesVec.push_back(currentDir / "xml");
    candidatesVec.push_back(executableDir / "xml");
    candidatesVec.push_back(currentDir / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(currentDir.parent_path() / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(currentDir / "worker" / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(currentDir.parent_path() / "worker" / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(executableDir / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(executableDir.parent_path() / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(executableDir / "worker" / "gutenprint" / "src" / "xml");
    candidatesVec.push_back(executableDir.parent_path() / "worker" / "gutenprint" / "src" / "xml");

    for (auto dir = executableDir; dir.has_parent_path() && dir != dir.parent_path(); dir = dir.parent_path()) {
        candidatesVec.push_back(dir / "worker" / "gutenprint" / "src" / "xml");
    }
    for (auto dir = currentDir; dir.has_parent_path() && dir != dir.parent_path(); dir = dir.parent_path()) {
        candidatesVec.push_back(dir / "worker" / "gutenprint" / "src" / "xml");
    }

    for (const auto& candidate : candidatesVec) {
        if (xmlDataPathContainsLocalDrivers(candidate)) {
            setEnv("STP_DATA_PATH", candidate);
            std::cout << "STP_DATA_PATH=" << candidate.string() << '\n';
            return;
        }
    }

    std::cerr << "STP_DATA_PATH is not set and xml was not found near the worker.\n";
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
