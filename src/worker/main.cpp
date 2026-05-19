#include "rip_worker_service.h"

#include <grpcpp/grpcpp.h>

#include <QCoreApplication>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

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

void configureGutenprintDataPath(const char* executablePath)
{
    if (envIsSet("STP_DATA_PATH")) {
        return;
    }

    const std::filesystem::path executableDir = std::filesystem::absolute(executablePath).parent_path();
    const std::filesystem::path currentDir = std::filesystem::current_path();
    const std::filesystem::path candidates[] = {
        currentDir.parent_path() / "xml",
        executableDir.parent_path() / "xml",
        currentDir / "xml",
        executableDir / "xml",
        currentDir / "gutenprint" / "src" / "xml",
        currentDir.parent_path() / "gutenprint" / "src" / "xml",
        currentDir / "worker" / "gutenprint" / "src" / "xml",
        currentDir.parent_path() / "worker" / "gutenprint" / "src" / "xml",
        executableDir / "gutenprint" / "src" / "xml",
        executableDir.parent_path() / "gutenprint" / "src" / "xml",
        executableDir / "worker" / "gutenprint" / "src" / "xml",
        executableDir.parent_path() / "worker" / "gutenprint" / "src" / "xml",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "printers" / "escp2.xml")) {
            setEnv("STP_DATA_PATH", candidate);
            std::cout << "STP_DATA_PATH=" << candidate.string() << '\n';
            return;
        }
    }

    std::cerr << "STP_DATA_PATH is not set and xml was not found near the worker.\n";
}

QString pathToQString(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

void configureQtPluginPaths(const char* executablePath)
{
    const std::filesystem::path executableDir = std::filesystem::absolute(executablePath).parent_path();
    const std::filesystem::path candidates[] = {
        executableDir,
        executableDir / "plugins",
        executableDir.parent_path() / "plugins",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "imageformats")) {
            QCoreApplication::addLibraryPath(pathToQString(candidate));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:50051";
    configureQtPluginPaths(argv[0]);
    configureGutenprintDataPath(argv[0]);

    RipWorkerService service;
    grpc::ServerBuilder builder;
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
