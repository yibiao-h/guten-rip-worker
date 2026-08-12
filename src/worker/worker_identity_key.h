#pragma once

// 构建时生成：worker Ed25519 身份密钥（raw private 与同源 raw public）。
// 生成物头文件由 cmake/worker_keys.cmake 通过 target_include_directories 注入；
// 私钥只允许出现在 worker 进程中，禁止进入主程序。

#include "worker_private_key.h"
#include "worker_public_key.h"
