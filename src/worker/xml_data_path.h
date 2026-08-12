#pragma once

#include <filesystem>
#include <optional>
#include <string>

// 解析 print engine XML 数据目录（STP_DATA_PATH）。
// 优先级：
//   1. 环境变量 STP_DATA_PATH：仅当规范化后属于仓库 worker/gutenprint/src/xml
//      或安装 xml/ 目录时接受；否则忽略并给出警告。调试构建可用
//      YINK_WORKER_ALLOW_ARBITRARY_XML=1 显式放行。
//   2. executableDir / cwd 附近的候选目录 + 向上回溯（统一 main 与 service 的旧逻辑）。
// 返回空表示未找到可用的 XML 数据目录。
std::optional<std::filesystem::path> findRepositoryXmlDataPath(
    const std::filesystem::path& executableDir,
    const std::filesystem::path& cwd,
    std::string* warning = nullptr);
