#include "native.hpp"
#include "catalog.hpp"
#include "miniz/miniz.h"
#include <curl/curl.h>
#include <array>
#include <cerrno>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace gs {
namespace {
constexpr uint64_t MAX_ARCHIVE = 1024ULL * 1024 * 1024;
constexpr uint64_t MAX_EXPANDED = 4ULL * 1024 * 1024 * 1024;
void Label(const std::string &name) {
    static const std::regex valid("[a-z0-9][a-z0-9._-]{0,95}");
    if (!std::regex_match(name, valid)) throw std::runtime_error("Invalid release identifier");
}
bool IsHash(const std::string &value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void WriteJSON(const fs::path &path, const json &data) {
    std::ofstream out(path);
    out << Canonical(data);
    out.close();
    if (!out) throw std::runtime_error("Cannot write " + path.string());
}
json PayloadFiles(const fs::path &path) {
    if (!fs::is_directory(path) || fs::is_symlink(path)) throw std::runtime_error("Missing or unsafe payload directory");
    json files = json::object();
    for (const auto &item : fs::recursive_directory_iterator(path)) {
        if (item.is_symlink()) throw std::runtime_error("Bundle symlinks are not supported");
        if (item.is_regular_file()) files[item.path().lexically_relative(path).generic_string()] = Hash(item.path());
        else if (!item.is_directory()) throw std::runtime_error("Bundle special files are not supported");
    }
    return files;
}
struct Stage {
    fs::path path;
    explicit Stage(const fs::path &root) {
        auto name = (root / ".install-XXXXXX").string();
        if (!mkdtemp(name.data())) throw std::runtime_error("Cannot create install staging directory");
        path = name;
    }
    ~Stage() { std::error_code ec; fs::remove_all(path, ec); }
};
struct Download {
    std::ofstream out;
    uint64_t size = 0;
    explicit Download(const fs::path &path) : out(path, std::ios::binary) {}
};
size_t DownloadBytes(char *data, size_t size, size_t count, void *opaque) noexcept {
    auto &sink = *static_cast<Download *>(opaque);
    if (size && count > MAX_ARCHIVE / size) return 0;
    size_t bytes = size * count;
    if (bytes > MAX_ARCHIVE - sink.size) return 0;
    sink.out.write(data, bytes);
    if (!sink.out) return 0;
    sink.size += bytes;
    return bytes;
}
void Acquire(const json &spec, const fs::path &destination, const std::string &local) {
    const std::string expected = spec.at("sha256");
    if (!IsHash(expected)) throw std::runtime_error("Archive requires a SHA-256 pin");
    if (!local.empty()) {
        auto source = Expand(local);
        if (!fs::is_regular_file(source) || fs::file_size(source) > MAX_ARCHIVE)
            throw std::runtime_error("Archive exceeds download limit or is not a file");
        fs::copy_file(source, destination);
    } else {
        const std::string url = spec.at("url");
        if (url.rfind("https://", 0) != 0) throw std::runtime_error("Downloads must use HTTPS");
        static const bool initialized = [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Cannot initialize HTTPS");
            return true;
        }();
        (void)initialized;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Cannot initialize download");
        Download sink(destination);
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 60L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, DownloadBytes);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
        auto code = curl_easy_perform(curl.get());
        sink.out.close();
        if (code != CURLE_OK || !sink.out) throw std::runtime_error(std::string("Archive download failed: ") + curl_easy_strerror(code));
    }
    if (Hash(destination) != expected) throw std::runtime_error("Downloaded/imported archive checksum mismatch");
}
} // namespace

void SafeExtract(const fs::path &archive, const fs::path &destination) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.c_str(), 0)) throw std::runtime_error("Cannot open ZIP archive");
    struct End { mz_zip_archive *zip; ~End() { mz_zip_reader_end(zip); } } end{&zip};
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > 20000) throw std::runtime_error("Archive exceeds extraction limits");
    uint64_t total = 0;
    std::set<std::string> names;
    struct Item { fs::path name; uint64_t size; bool directory; };
    std::vector<Item> items;
    for (mz_uint index = 0; index < count; index++) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) throw std::runtime_error("Invalid ZIP entry");
        if (stat.m_uncomp_size > MAX_EXPANDED - total) throw std::runtime_error("Archive exceeds extraction limits");
        total += stat.m_uncomp_size;
        auto length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        if (!length || length > 4096) throw std::runtime_error("Unsafe archive path length");
        std::string name(length, '\0');
        mz_zip_reader_get_filename(&zip, index, name.data(), length);
        name.resize(length - 1);
        fs::path path(name);
        if (name.empty() || name.find('\0') != std::string::npos || path.is_absolute() ||
            name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
            throw std::runtime_error("Unsafe archive path");
        for (const auto &part : path) if (part == "..") throw std::runtime_error("Unsafe archive path");
        auto type = (stat.m_external_attr >> 16) & S_IFMT;
        if (type == S_IFLNK) throw std::runtime_error("Archive symlinks are not supported");
        if (type && type != S_IFDIR && type != S_IFREG) throw std::runtime_error("Archive special files are not supported");
        path = path.lexically_normal();
        if (path.filename().empty()) path = path.parent_path();
        if (path.empty() || path == "." || !names.insert(path.generic_string()).second)
            throw std::runtime_error("Duplicate or unsafe archive entry");
        items.push_back({path, stat.m_uncomp_size, mz_zip_reader_is_file_a_directory(&zip, index) != 0});
    }
    // Destination is always an empty, private staging directory.
    if (fs::exists(destination) && (!fs::is_directory(destination) || !fs::is_empty(destination) || fs::is_symlink(destination)))
        throw std::runtime_error("Extraction requires an empty private directory");
    fs::create_directories(destination);
    for (mz_uint index = 0; index < count; index++) {
        const auto &item = items[index];
        auto target = destination / item.name;
        if (item.directory) { fs::create_directories(target); continue; }
        fs::create_directories(target.parent_path());
        struct Sink { std::ofstream out; uint64_t size = 0, limit; } sink{std::ofstream(target, std::ios::binary), 0, item.size};
        auto callback = [](void *opaque, mz_uint64 offset, const void *data, size_t bytes) -> size_t {
            auto &s = *static_cast<Sink *>(opaque);
            if (offset != s.size || bytes > s.limit - s.size) return 0;
            s.out.write(static_cast<const char *>(data), bytes);
            if (!s.out) return 0;
            s.size += bytes;
            return bytes;
        };
        if (!mz_zip_reader_extract_to_callback(&zip, index, callback, &sink, 0) || sink.size != item.size)
            throw std::runtime_error("ZIP extraction or CRC validation failed");
        sink.out.close();
        if (!sink.out) throw std::runtime_error("ZIP output write failed");
    }
}

Store::Store() : root(Expand(Env("GEOSUPPORT_HOME", "~/.cache/duckdb-geosupport"))), catalog_path(Env("GEOSUPPORT_CATALOG")) {}
json Store::Catalog() const {
    auto data = catalog_path.empty() ? json::parse(CATALOG_JSON) : ReadJSON(Expand(catalog_path));
    if (data.at("schema_version") != 1) throw std::runtime_error("Unsupported catalog schema");
    return data.at("releases");
}
json Store::Available() const {
    auto out = json::array();
    for (const auto &entry : Catalog()) out.push_back(entry);
    return out;
}
std::pair<fs::path, json> Store::Resolve(const std::string &name) const {
    Label(name);
    std::string id = name;
    if (!IsHash(id)) {
        auto alias = root / "aliases" / (name + ".json");
        if (!fs::exists(alias)) throw std::runtime_error("Release " + name + " is not installed; use gs_install or gs_import explicitly");
        id = ReadJSON(alias).at("bundle_id");
    }
    if (!IsHash(id)) throw std::runtime_error("Invalid bundle identity");
    auto path = root / "bundles" / id;
    auto manifest = ReadJSON(path / "manifest.json");
    if (HashBytes(Canonical(manifest.at("identity"))) != id || manifest.at("bundle_id") != id)
        throw std::runtime_error("Bundle manifest identity mismatch");
    return {path, manifest};
}
json Store::Verify(const std::string &name) const {
    auto resolved = Resolve(name);
    if (resolved.second.at("identity").at("format_version") != 1) throw std::runtime_error("Unsupported bundle format");
    if (PayloadFiles(resolved.first / "payload") != resolved.second.at("identity").at("files"))
        throw std::runtime_error("Bundle " + name + " payload checksum mismatch");
    return resolved.second;
}
json Store::Describe(const json &manifest) {
    const auto &identity = manifest.at("identity");
    return {{"release", identity.at("release")}, {"bundle_id", manifest.at("bundle_id")},
        {"engine_version", identity.at("engine_version")}, {"platform", identity.at("platform")},
        {"patches", identity.at("patches")}, {"verified", true}, {"adapter", "native-c++"}, {"extension_version", VERSION},
        {"layout_source_commit", "82c39e82b4d4dc7773fa67d32ef7f3787ef4d27e"}};
}
json Store::Installed() const {
    json out = json::array();
    if (!fs::exists(root / "bundles")) return out;
    std::vector<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(root / "bundles")) {
        if (fs::exists(entry.path() / "manifest.json")) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) {
        auto row = Describe(Resolve(path.filename()).second);
        row["verified"] = false;
        out.push_back(row);
    }
    return out;
}
json Store::Install(const std::string &name, const std::string &local) const {
    Label(name);
    auto catalog = Catalog();
    if (!catalog.contains(name)) throw std::runtime_error("Release " + name + " is not in the pinned catalog");
    auto entry = catalog.at(name);
    auto patches = entry.value("patches", json::array());
    json archives = json::array({entry.at("archive").at("sha256")}), patch_ids = json::array();
    for (const auto &patch : patches) { archives.push_back(patch.at("sha256")); patch_ids.push_back(patch.at("sha256")); }
    if (fs::exists(root / "aliases" / (name + ".json"))) {
        auto manifest = Verify(name);
        if (manifest.at("identity").at("archives") != archives) throw std::runtime_error("Release label already refers to different bytes; use a new release label");
        return Describe(manifest);
    }
    if (!local.empty() && !patches.empty()) throw std::runtime_error("Offline import of patched releases is unsupported; import base releases only");
    fs::create_directories(root / "bundles");
    fs::create_directories(root / "aliases");
    Stage stage(root);
    Acquire(entry.at("archive"), stage.path / "base.zip", local);
    auto unpack = stage.path / "unpack";
    SafeExtract(stage.path / "base.zip", unpack);
    std::vector<fs::path> roots;
    for (const auto &file : fs::recursive_directory_iterator(unpack)) {
        if (file.path().filename() == "libgeo.so" && file.path().parent_path().filename() == "lib")
            roots.push_back(file.path().parent_path().parent_path());
    }
    if (roots.size() != 1 || !fs::is_directory(roots[0] / "fls"))
        throw std::runtime_error("Expected exactly one Linux Geosupport lib/ and fls/ installation");
    auto publish = stage.path / "publish", payload = publish / "payload";
    fs::create_directory(publish);
    fs::rename(roots[0], payload);
    for (size_t n = 0; n < patches.size(); n++) {
        auto zip = stage.path / ("patch-" + std::to_string(n) + ".zip");
        auto directory = stage.path / ("patch-" + std::to_string(n));
        Acquire(patches[n], zip, "");
        SafeExtract(zip, directory);
        for (const auto &file : fs::recursive_directory_iterator(directory)) if (file.is_regular_file()) {
            auto target = payload / "fls" / file.path().lexically_relative(directory);
            fs::create_directories(target.parent_path());
            fs::copy_file(file.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    json identity = {{"format_version", 1}, {"release", name}, {"engine_version", entry.at("engine_version")},
        {"platform", entry.at("platform")}, {"patches", patch_ids}, {"archives", archives}, {"files", PayloadFiles(payload)}};
    auto id = HashBytes(Canonical(identity));
    json manifest = {{"bundle_id", id}, {"identity", identity}, {"sources", entry}};
    WriteJSON(publish / "manifest.json", manifest);
    auto destination = root / "bundles" / id;
    std::error_code error;
    fs::rename(publish, destination, error);
    if (error) { if (!fs::exists(destination)) throw fs::filesystem_error("Cannot publish bundle", publish, destination, error); Verify(id); }
    auto alias = root / "aliases" / (name + ".json");
    WriteJSON(stage.path / "alias.json", {{"bundle_id", id}});
    if (link((stage.path / "alias.json").c_str(), alias.c_str()) != 0) {
        if (errno != EEXIST) throw std::runtime_error("Cannot publish release alias");
        if (ReadJSON(alias).at("bundle_id") != id) throw std::runtime_error("Concurrent installation published different bytes for this release");
    }
    return Describe(manifest);
}
} // namespace gs
