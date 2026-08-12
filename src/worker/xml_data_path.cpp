#include "xml_data_path.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool xmlDataPathContainsLocalDrivers(const std::filesystem::path& dataPath)
{
    const std::filesystem::path escp2 = dataPath / "printers" / "escp2.xml";
    std::error_code ec;
    return std::filesystem::is_regular_file(escp2, ec) && !ec &&
           std::filesystem::file_size(escp2, ec) > 0 && !ec;
}

// 规范化（解析 junction/相对路径）后路径必须属于仓库
// worker/gutenprint/src/xml 或安装 xml/ 目录。
bool isAllowedXmlDataPath(const std::filesystem::path& dataPath)
{
    std::error_code ec;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(dataPath, ec);
    if (ec) {
        return false;
    }
    std::vector<std::string> components;
    for (const auto& part : canonical) {
        components.push_back(part.string());
    }
    const std::size_t count = components.size();
    if (count >= 4 &&
        components[count - 1] == "xml" &&
        components[count - 2] == "src" &&
        components[count - 3] == "gutenprint" &&
        components[count - 4] == "worker") {
        return true;  // 仓库布局
    }
    if (count >= 1 && components[count - 1] == "xml") {
        return true;  // 安装布局
    }
    return false;
}

bool envIsSet(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0';
}

} // namespace

std::optional<std::filesystem::path> findRepositoryXmlDataPath(
    const std::filesystem::path& executableDir,
    const std::filesystem::path& cwd,
    std::string* warning)
{
    if (const char* envValue = std::getenv("STP_DATA_PATH")) {
        if (envValue[0] != '\0') {
            const std::filesystem::path envPath(envValue);
            const bool structurallyAllowed = isAllowedXmlDataPath(envPath);
            // 开发放行只对调试构建有效，发布构建编译期禁用。
#ifndef NDEBUG
            const bool devOverride = envIsSet("YINK_WORKER_ALLOW_ARBITRARY_XML");
#else
            const bool devOverride = false;
#endif
            if ((structurallyAllowed || devOverride) &&
                xmlDataPathContainsLocalDrivers(envPath)) {
                return envPath;
            }
            if (warning) {
                *warning = structurallyAllowed
                    ? "STP_DATA_PATH is set but does not contain local print drivers; ignored."
                    : "STP_DATA_PATH is set to a non-repository XML path; ignored.";
            }
        }
    }

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(cwd.parent_path() / "xml");
    candidates.push_back(executableDir.parent_path() / "xml");
    candidates.push_back(cwd / "xml");
    candidates.push_back(executableDir / "xml");
    candidates.push_back(cwd / "gutenprint" / "src" / "xml");
    candidates.push_back(cwd.parent_path() / "gutenprint" / "src" / "xml");
    candidates.push_back(cwd / "worker" / "gutenprint" / "src" / "xml");
    candidates.push_back(cwd.parent_path() / "worker" / "gutenprint" / "src" / "xml");
    candidates.push_back(executableDir / "gutenprint" / "src" / "xml");
    candidates.push_back(executableDir.parent_path() / "gutenprint" / "src" / "xml");
    candidates.push_back(executableDir / "worker" / "gutenprint" / "src" / "xml");
    candidates.push_back(executableDir.parent_path() / "worker" / "gutenprint" / "src" / "xml");
    for (auto dir = executableDir;
         dir.has_parent_path() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        candidates.push_back(dir / "worker" / "gutenprint" / "src" / "xml");
    }
    for (auto dir = cwd;
         dir.has_parent_path() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        candidates.push_back(dir / "worker" / "gutenprint" / "src" / "xml");
    }

    for (const auto& candidate : candidates) {
        if (xmlDataPathContainsLocalDrivers(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}
