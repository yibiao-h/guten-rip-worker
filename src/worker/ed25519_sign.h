// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Yink Studio contributors

#pragma once

#include <cstddef>
#include <vector>

// Ed25519 raw key 签名/验签（worker 身份握手），OpenSSL EVP 实现。
// 命名使用产品术语，不引用任何依赖名。

// 用 raw private key（32 字节）对 message 签名，返回 64 字节签名；失败返回空。
std::vector<unsigned char> workerSignEd25519(const unsigned char* rawPrivateKey,
                                             std::size_t keySize,
                                             const unsigned char* message,
                                             std::size_t messageSize);

// 用 raw public key（32 字节）验签；签名必须为 64 字节。
bool workerVerifyEd25519(const unsigned char* rawPublicKey,
                         std::size_t keySize,
                         const unsigned char* message,
                         std::size_t messageSize,
                         const unsigned char* signature,
                         std::size_t signatureSize);
