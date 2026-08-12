#include "ed25519_sign.h"

#include <openssl/evp.h>

#include <cstdint>

namespace {

constexpr std::size_t kRawKeyBytes = 32;
constexpr std::size_t kSignatureBytes = 64;

} // namespace

std::vector<unsigned char> workerSignEd25519(const unsigned char* rawPrivateKey,
                                             std::size_t keySize,
                                             const unsigned char* message,
                                             std::size_t messageSize)
{
    if (!rawPrivateKey || keySize != kRawKeyBytes ||
        !message || messageSize == 0) {
        return {};
    }

    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, rawPrivateKey, keySize);
    if (!key) {
        return {};
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        EVP_PKEY_free(key);
        return {};
    }

    std::vector<unsigned char> signature;
    if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) != 1) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return {};
    }

    std::size_t signatureSize = 0;
    if (EVP_DigestSign(context, nullptr, &signatureSize, message, messageSize) != 1 ||
        signatureSize != kSignatureBytes) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return {};
    }
    signature.resize(signatureSize);
    const int signOk = EVP_DigestSign(context,
                                      signature.data(),
                                      &signatureSize,
                                      message,
                                      messageSize);
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (signOk != 1) {
        return {};
    }
    signature.resize(signatureSize);
    return signature;
}

bool workerVerifyEd25519(const unsigned char* rawPublicKey,
                         std::size_t keySize,
                         const unsigned char* message,
                         std::size_t messageSize,
                         const unsigned char* signature,
                         std::size_t signatureSize)
{
    if (!rawPublicKey || keySize != kRawKeyBytes ||
        !message || messageSize == 0 ||
        !signature || signatureSize != kSignatureBytes) {
        return false;
    }

    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, rawPublicKey, keySize);
    if (!key) {
        return false;
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        EVP_PKEY_free(key);
        return false;
    }

    const int initOk = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key);
    const int verifyOk = initOk == 1
        ? EVP_DigestVerify(context, signature, signatureSize, message, messageSize)
        : 0;

    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return verifyOk == 1;
}
