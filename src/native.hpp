#pragma once
#include "nlohmann/json.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace gs {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr size_t MAX_FRAME = 128 * 1024 * 1024;
constexpr const char *VERSION = "0.2.0";
std::string Canonical(const json &value);
std::string Hash(const fs::path &path);
std::string HashBytes(const std::string &bytes);
json ReadJSON(const fs::path &path);
std::string Env(const char *key, const std::string &fallback = "");
fs::path Expand(const std::string &path);
void SafeExtract(const fs::path &archive, const fs::path &destination);

class Store {
public:
    fs::path root;
    explicit Store();
    json Catalog() const;
    json Available() const;
    std::pair<fs::path, json> Resolve(const std::string &name) const;
    json Verify(const std::string &name) const;
    json Installed() const;
    json Install(const std::string &name, const std::string &local = "") const;
    static json Describe(const json &manifest);
private:
    std::string catalog_path;
};

class Manager {
public:
    Manager();
    ~Manager();
    json Request(const json &request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::mutex mutex;
};

// Used by the native worker and a test harness; no Python code is invoked.
class Engine {
public:
    explicit Engine(const fs::path &bundle);
    ~Engine();
    json Call(const json &request);
    json Batch(const json &requests);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
int EngineMain(const fs::path &bundle);
} // namespace gs
