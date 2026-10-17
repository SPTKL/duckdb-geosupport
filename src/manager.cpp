#include "native.hpp"
#include "worker.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace gs {
namespace {
[[maybe_unused]] fs::path MaterializeWorker(const fs::path &root) {
    auto directory = root / "native";
    fs::create_directories(directory);
    struct stat stat{};
    if (lstat(directory.c_str(), &stat) || !S_ISDIR(stat.st_mode) || stat.st_uid != geteuid())
        throw std::runtime_error("Native worker cache must be a directory owned by the current user");
    if (chmod(directory.c_str(), 0700) != 0) throw std::runtime_error("Cannot secure native worker cache");
    auto target = directory / WORKER_SHA256;
    if (!fs::exists(target)) {
        auto temporary = (directory / ".worker-XXXXXX").string();
        int fd = mkstemp(temporary.data());
        if (fd < 0) throw std::runtime_error("Cannot stage native worker");
        try {
            size_t offset = 0;
            while (offset < WORKER_SIZE) {
                auto n = write(fd, WORKER_BYTES + offset, WORKER_SIZE - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("Cannot write native worker");
                offset += static_cast<size_t>(n);
            }
            if (fchmod(fd, 0500) || fsync(fd)) throw std::runtime_error("Cannot finalize native worker");
            close(fd); fd = -1;
            if (link(temporary.c_str(), target.c_str()) != 0 && errno != EEXIST)
                throw std::runtime_error("Cannot publish native worker");
            unlink(temporary.c_str());
        } catch (...) { if (fd >= 0) close(fd); unlink(temporary.c_str()); throw; }
    }
    if (lstat(target.c_str(), &stat) || !S_ISREG(stat.st_mode) || stat.st_uid != geteuid() ||
        (stat.st_mode & 0077) || !(stat.st_mode & S_IXUSR) || Hash(target) != WORKER_SHA256)
        throw std::runtime_error("Native worker cache integrity or permission mismatch");
    return target;
}

class Worker {
    int fd = -1;
    pid_t pid = -1;
    std::string buffered;
    using Deadline = std::chrono::steady_clock::time_point;
    void Wait(short event, Deadline deadline) {
        while (true) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("Geosupport worker timed out (300 seconds)");
            pollfd descriptor{fd, event, 0};
            int result = poll(&descriptor, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) throw std::runtime_error("Worker poll failed");
            if (!result) continue;
            if (descriptor.revents & event) return;
            throw std::runtime_error("Geosupport worker disconnected; inspect stderr");
        }
    }
    json Read(Deadline deadline) {
        while (buffered.find('\n') == std::string::npos) {
            Wait(POLLIN, deadline);
            char data[65536];
            auto n = recv(fd, data, sizeof(data), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) throw std::runtime_error("Geosupport worker exited before responding; inspect stderr");
            buffered.append(data, n);
            if (buffered.size() > MAX_FRAME) throw std::runtime_error("Worker response exceeds protocol limit");
        }
        auto end = buffered.find('\n');
        auto response = json::parse(buffered.substr(0, end));
        buffered.erase(0, end + 1);
        if (response.contains("error")) throw std::runtime_error(response.at("error").get<std::string>());
        return response;
    }
public:
    json manifest;
    size_t last_use = 0;
    Worker(const fs::path &bundle, const json &data, const fs::path &store) : manifest(data) {
        auto runtime = Env("GEOSUPPORT_RUNTIME", "local");
        std::vector<std::string> command, environment;
        for (char **item = environ; *item; item++) {
            std::string value(*item);
            if (value.rfind("GEOFILES=", 0) != 0 && value.rfind("LD_LIBRARY_PATH=", 0) != 0) environment.push_back(value);
        }
        if (runtime == "docker") {
            command = {Env("GEOSUPPORT_DOCKER", "docker"), "run", "--rm", "-i", "--pull=never", "--platform", "linux/amd64",
                "--network=none", "--read-only", "--mount", "type=bind,src=" + bundle.string() + ",dst=/bundle,readonly",
                "-e", "GEOFILES=/bundle/payload/fls/", "-e", "LD_LIBRARY_PATH=/bundle/payload/lib",
                Env("GEOSUPPORT_DOCKER_IMAGE", "duckdb-geosupport-runtime:0.2.0"), "--engine", "/bundle"};
        } else if (runtime == "local") {
#if defined(__linux__) && defined(__x86_64__)
            command = {MaterializeWorker(store).string(), "--engine", bundle.string()};
            environment.push_back("GEOFILES=" + (bundle / "payload/fls").string() + "/");
            environment.push_back("LD_LIBRARY_PATH=" + (bundle / "payload/lib").string());
#else
            (void)store;
            throw std::runtime_error("Local Geosupport requires Linux x86-64; on macOS select GEOSUPPORT_RUNTIME=docker");
#endif
        } else throw std::runtime_error("GEOSUPPORT_RUNTIME must be local or docker");
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("socketpair failed");
        fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
        fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
#ifdef __APPLE__
        int yes = 1;
        setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
        std::vector<char *> argv, envp;
        for (auto &item : command) argv.push_back(item.data());
        for (auto &item : environment) envp.push_back(item.data());
        argv.push_back(nullptr); envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        int error = posix_spawnp(&pid, command[0].c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (error) { close(sockets[0]); pid = -1; throw std::runtime_error("Cannot start native worker: " + std::string(strerror(error))); }
        fd = sockets[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        try {
            auto ready = Read(std::chrono::steady_clock::now() + std::chrono::seconds(300));
            if (ready.value("ready", json(nullptr)) != manifest.at("bundle_id")) throw std::runtime_error("Worker startup identity mismatch");
        } catch (...) { Stop(); throw; }
    }
    ~Worker() { Stop(); }
    void Stop() noexcept {
        if (fd >= 0) { shutdown(fd, SHUT_WR); close(fd); fd = -1; }
        if (pid > 0) {
            for (int n = 0; n < 30; n++) {
                auto result = waitpid(pid, nullptr, WNOHANG);
                if (result == pid || (result < 0 && errno == ECHILD)) { pid = -1; return; }
                usleep(100000);
            }
            kill(pid, SIGTERM);
            for (int n = 0; n < 20; n++) {
                if (waitpid(pid, nullptr, WNOHANG) == pid) { pid = -1; return; }
                usleep(100000);
            }
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            pid = -1;
        }
    }
    json Call(const json &requests) {
        auto bytes = Canonical(requests) + "\n";
        if (bytes.size() > MAX_FRAME) throw std::runtime_error("Request exceeds protocol limit");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
        size_t offset = 0;
        while (offset < bytes.size()) {
            Wait(POLLOUT, deadline);
#ifdef MSG_NOSIGNAL
            auto n = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#else
            auto n = send(fd, bytes.data() + offset, bytes.size() - offset, 0);
#endif
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) throw std::runtime_error("Worker write failed");
            offset += static_cast<size_t>(n);
        }
        return Read(deadline).at("results");
    }
};
} // namespace

struct Manager::Impl {
    Store store;
    std::map<std::string, std::unique_ptr<Worker>> workers;
    size_t clock = 0;
    Worker &Load(const std::string &release) {
        auto resolved = store.Resolve(release);
        auto id = resolved.second.at("bundle_id").get<std::string>();
        if (!workers.count(id)) {
            store.Verify(id);
            if (resolved.second.at("identity").at("platform") != "linux_amd64") throw std::runtime_error("Unsupported bundle platform");
            // Bound native resources; installed bundles are never removed by eviction.
            if (workers.size() >= 8) {
                auto oldest = workers.begin();
                for (auto it = workers.begin(); it != workers.end(); ++it)
                    if (it->second->last_use < oldest->second->last_use) oldest = it;
                workers.erase(oldest);
            }
            workers.emplace(id, std::make_unique<Worker>(resolved.first, resolved.second, store.root));
        }
        workers.at(id)->last_use = ++clock;
        return *workers.at(id);
    }
    json Dispatch(const json &request) {
        std::string op = request.at("op");
        auto args = request.value("args", json::array());
        if (op == "available") return store.Available();
        if (op == "installed") return store.Installed();
        if (op == "install" || op == "import") return json::array({store.Install(args.at(0), args.size() > 1 ? args.at(1).get<std::string>() : "")});
        if (op == "verify") return json::array({Store::Describe(store.Verify(args.at(0)))});
        if (op == "load") return json::array({Store::Describe(Load(args.at(0)).manifest)});
        if (op == "unload") {
            auto resolved = store.Resolve(args.at(0));
            workers.erase(resolved.second.at("bundle_id").get<std::string>());
            return json::array({Store::Describe(resolved.second)});
        }
        if (op == "runtime_info") {
            auto out = json::array();
            for (const auto &item : workers) out.push_back(Store::Describe(item.second->manifest));
            return out;
        }
        if (op == "batch") {
            const auto &rows = request.at("rows");
            std::map<std::string, std::vector<size_t>> groups;
            auto results = json::array();
            for (size_t n = 0; n < rows.size(); n++) {
                results.push_back(nullptr);
                if (!rows[n].is_null()) groups[rows[n].at("release").get<std::string>()].push_back(n);
            }
            for (const auto &group : groups) {
                auto &worker = Load(group.first);
                auto id = worker.manifest.at("bundle_id").get<std::string>();
                auto unique = json::array();
                std::map<std::string, size_t> seen;
                std::vector<size_t> indices;
                for (auto n : group.second) {
                    auto key = Canonical(rows[n]);
                    auto found = seen.find(key);
                    if (found == seen.end()) { seen[key] = unique.size(); indices.push_back(unique.size()); unique.push_back(rows[n]); }
                    else indices.push_back(found->second);
                }
                try {
                    auto values = worker.Call(unique);
                    if (!values.is_array() || values.size() != unique.size()) throw std::runtime_error("Worker returned wrong row count");
                    for (size_t n = 0; n < group.second.size(); n++) results[group.second[n]] = values[indices[n]];
                } catch (...) { workers.erase(id); throw; }
            }
            return results;
        }
        throw std::runtime_error("Unknown operation " + op);
    }
};
Manager::Manager() = default;
Manager::~Manager() = default;
json Manager::Request(const json &request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!impl) impl = std::make_unique<Impl>();
    return impl->Dispatch(request);
}
} // namespace gs
