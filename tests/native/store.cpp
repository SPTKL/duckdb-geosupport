#include "native.hpp"
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace gs;
namespace {
int checks = 0;
void Check(bool ok, const std::string &label) {
    if (!ok) throw std::runtime_error(label);
    ++checks;
}
void Fails(const std::function<void()> &fn, const std::string &message) {
    try { fn(); } catch (const std::exception &e) {
        Check(std::string(e.what()).find(message) != std::string::npos, "Expected " + message + ", got " + e.what()); return;
    }
    throw std::runtime_error("Expected failure: " + message);
}
void Write(const fs::path &path, const std::string &bytes) { std::ofstream(path, std::ios::binary) << bytes; }
// Minimal stored ZIP fixture writer. Never used for a real engine or data.
void LE(std::ostream &out, uint32_t n, int width) { for (int i = 0; i < width; i++) out.put(static_cast<char>(n >> (8 * i))); }
uint32_t CRC(const std::string &data) {
    uint32_t crc = ~0U;
    for (unsigned char b : data) { crc ^= b; for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1))); }
    return ~crc;
}
struct Entry { std::string name, data; uint32_t mode = S_IFREG | 0644; };
void Zip(const fs::path &path, const std::vector<Entry> &entries) {
    std::ofstream out(path, std::ios::binary);
    std::vector<uint32_t> offsets;
    for (const auto &e : entries) {
        offsets.push_back(static_cast<uint32_t>(out.tellp()));
        LE(out, 0x04034b50, 4); LE(out, 20, 2);
        for (int i = 0; i < 4; i++) LE(out, 0, 2);
        LE(out, CRC(e.data), 4); LE(out, e.data.size(), 4); LE(out, e.data.size(), 4);
        LE(out, e.name.size(), 2); LE(out, 0, 2); out << e.name << e.data;
    }
    auto start = static_cast<uint32_t>(out.tellp());
    size_t i = 0;
    for (const auto &e : entries) {
        LE(out, 0x02014b50, 4); LE(out, (3 << 8) | 20, 2); LE(out, 20, 2);
        for (int k = 0; k < 4; k++) LE(out, 0, 2);
        LE(out, CRC(e.data), 4); LE(out, e.data.size(), 4); LE(out, e.data.size(), 4);
        LE(out, e.name.size(), 2);
        for (int k = 0; k < 4; k++) LE(out, 0, 2);
        LE(out, e.mode << 16, 4); LE(out, offsets[i++], 4); out << e.name;
    }
    auto size = static_cast<uint32_t>(out.tellp()) - start;
    LE(out, 0x06054b50, 4); LE(out, 0, 2); LE(out, 0, 2);
    LE(out, entries.size(), 2); LE(out, entries.size(), 2); LE(out, size, 4); LE(out, start, 4); LE(out, 0, 2);
}
}
int main() {
    std::string name = (fs::temp_directory_path() / "geosupport-native-store-XXXXXX").string();
    if (!mkdtemp(name.data())) return 1;
    fs::path root(name);
    try {
        const auto archive = root / "bundle.zip", catalog = root / "catalog.json";
        Zip(archive, {{"version/lib/libgeo.so", "synthetic, never executed"}, {"version/fls/PAD", "synthetic data"}});
        json entry = {{"release", "test"}, {"engine_version", "test"}, {"platform", "linux_amd64"},
            {"archive", {{"url", "https://example.invalid/test.zip"}, {"sha256", Hash(archive)}}}, {"patches", json::array()}};
        json spec = {{"schema_version", 1}, {"releases", {{"test", entry}}}};
        Write(catalog, spec.dump());
        setenv("GEOSUPPORT_HOME", (root / "store").c_str(), 1);
        setenv("GEOSUPPORT_CATALOG", catalog.c_str(), 1);
        Store store;
        Fails([&] { store.Resolve("test"); }, "not installed");
        Check(!fs::exists(store.root), "Resolve created a store");
        auto bad = root / "bad.zip"; Write(bad, "bad archive");
        Fails([&] { store.Install("test", bad.string()); }, "checksum mismatch");
        Check(store.Installed().empty(), "Bad hash published a bundle");
        Check(!fs::exists(store.root / "aliases/test.json"), "Bad hash published an alias");
        for (const auto &file : fs::directory_iterator(store.root))
            Check(file.path().filename().string().find(".install-") != 0, "Failed install left staging files");
        // Offline imports must succeed even with unusable proxy settings.
        setenv("HTTPS_PROXY", "http://127.0.0.1:1", 1); setenv("ALL_PROXY", "http://127.0.0.1:1", 1);
        auto a = std::async(std::launch::async, [&] { return store.Install("test", archive.string()); });
        auto b = std::async(std::launch::async, [&] { return store.Install("test", archive.string()); });
        auto first = a.get(); Check(first == b.get(), "Concurrent installs disagreed");
        Check(first == store.Install("test", archive.string()), "Install is not idempotent");
        Check(store.Installed().size() == 1 && !store.Installed()[0].at("verified").get<bool>(), "Inventory semantics");
        Check(store.Verify(first.at("bundle_id")).at("identity").at("release") == "test", "Verify by identity");
        auto bundle = store.Resolve("test").first;
        Write(bundle / "payload/fls/PAD", "changed");
        Fails([&] { store.Verify("test"); }, "checksum mismatch");
        Write(bundle / "payload/fls/PAD", "synthetic data");
        Write(bundle / "payload/extra", "extra");
        Fails([&] { store.Verify("test"); }, "checksum mismatch");
        fs::remove(bundle / "payload/extra");
        fs::create_symlink("fls/PAD", bundle / "payload/link");
        Fails([&] { store.Verify("test"); }, "symlinks"); fs::remove(bundle / "payload/link");
        auto manifest = ReadJSON(bundle / "manifest.json"), changed = manifest;
        changed["identity"]["engine_version"] = "changed"; Write(bundle / "manifest.json", changed.dump());
        Fails([&] { store.Resolve("test"); }, "identity mismatch"); Write(bundle / "manifest.json", manifest.dump());
        spec["releases"]["test"]["archive"]["sha256"] = std::string(64, 'a'); Write(catalog, spec.dump());
        Fails([&] { store.Install("test", archive.string()); }, "different bytes");
        for (const auto &path : {"../escape", "/absolute", "a\\escape", "C:drive"}) {
            Zip(bad, {{path, "bad"}});
            Fails([&] { SafeExtract(bad, root / "extract"); }, "Unsafe archive");
        }
        Zip(bad, {{"link", "../escape", S_IFLNK | 0777}});
        Fails([&] { SafeExtract(bad, root / "extract"); }, "symlinks");
        Zip(bad, {{"same", "a"}, {"same", "b"}});
        Fails([&] { SafeExtract(bad, root / "extract"); }, "Duplicate");
        Zip(bad, {{"a/file", "a"}, {"a/./file", "b"}});
        Fails([&] { SafeExtract(bad, root / "extract"); }, "Duplicate");
        Zip(bad, {{"pipe", "", S_IFIFO | 0600}});
        Fails([&] { SafeExtract(bad, root / "extract"); }, "special files");
        Zip(bad, {{"safe", "data"}});
        fs::create_directory(root / "outside");
        fs::create_directory_symlink(root / "outside", root / "extract");
        Fails([&] { SafeExtract(bad, root / "extract"); }, "private directory");
        Check(fs::is_empty(root / "outside"), "Extraction wrote through a symlink");
        fs::remove(root / "extract");
        std::ifstream in(archive, std::ios::binary);
        std::string corrupted{std::istreambuf_iterator<char>(in), {}};
        corrupted.at(corrupted.find("synthetic data")) = 'X'; Write(bad, corrupted);
        auto broken = entry; broken["release"] = "broken"; broken["archive"]["sha256"] = Hash(bad);
        spec["releases"]["broken"] = broken; Write(catalog, spec.dump());
        Fails([&] { store.Install("broken", bad.string()); }, "ZIP");
        Check(store.Installed().size() == 1 && !fs::exists(store.root / "aliases/broken.json"), "CRC failure published a bundle");
        for (const auto &file : fs::directory_iterator(store.root))
            Check(file.path().filename().string().find(".install-") != 0, "CRC failure left staging files");
        std::cout << "PASS native store: " << checks << " checks\n";
        fs::remove_all(root);
    } catch (const std::exception &e) {
        std::cerr << "FAIL native store: " << e.what() << " (fixtures: " << root << ")\n"; return 1;
    }
}
