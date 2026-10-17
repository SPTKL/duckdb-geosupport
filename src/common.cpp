#include "native.hpp"
#include "picosha2/picosha2.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#ifndef GS_WORKER_ONLY
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#endif

namespace gs {
std::string Canonical(const json &value) { return value.dump(-1, ' ', true); }
std::string Env(const char *key, const std::string &fallback) {
    auto value = std::getenv(key);
    return value ? std::string(value) : fallback;
}
fs::path Expand(const std::string &path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        auto home = Env("HOME");
        if (home.empty()) throw std::runtime_error("HOME is required to expand store path");
        return fs::absolute(fs::path(home) / (path.size() > 2 ? path.substr(2) : ""));
    }
    return fs::absolute(path);
}
#ifndef GS_WORKER_ONLY
std::string HashBytes(const std::string &bytes) { return picosha2::hash256_hex_string(bytes); }
std::string Hash(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
#ifdef __APPLE__
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
#else
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) throw std::runtime_error("Cannot initialize SHA-256");
#endif
    // DuckDB may call us on a thread with a small stack (notably on macOS).
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), buffer.size());
#ifdef __APPLE__
        CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(input.gcount()));
#else
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<size_t>(input.gcount())) != 1) throw std::runtime_error("SHA-256 update failed");
#endif
    }
    if (!input.eof()) throw std::runtime_error("File read failed: " + path.string());
    std::array<unsigned char, 32> digest;
#ifdef __APPLE__
    CC_SHA256_Final(digest.data(), &context);
#else
    unsigned int length;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 || length != digest.size()) throw std::runtime_error("SHA-256 finalization failed");
#endif
    return picosha2::bytes_to_hex_string(digest.begin(), digest.end());
}
#endif
json ReadJSON(const fs::path &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    if (fs::file_size(path) > MAX_FRAME) throw std::runtime_error("JSON file exceeds size limit");
    json data;
    input >> data;
    return data;
}
} // namespace gs
