// Field formatting/parsing semantics derived from MIT-licensed python-geosupport.
// See resources/PYTHON_GEOSUPPORT_LICENSE and THIRD_PARTY_NOTICES.md.
#include "native.hpp"
#include "resources.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <list>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace gs {
namespace {
const json &Spec() { static const json value = json::parse(LAYOUTS_JSON); return value; }
std::string Trim(std::string value) {
    auto space = [](unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); };
    while (!value.empty() && space(value.back())) value.pop_back();
    size_t begin = 0;
    while (begin < value.size() && space(value[begin])) ++begin;
    return value.substr(begin);
}
std::string Upper(std::string value) {
    for (auto &c : value) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return value;
}
std::string Text(const json &value) {
    if (value.is_null()) return "";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    return value.dump();
}
bool Truth(const json &v) {
    return !v.is_null() && !(v.is_boolean() && !v.get<bool>()) &&
           !(v.is_number() && v == 0) && !(v.is_string() && v.get<std::string>().empty());
}
std::string Function(const std::string &value) {
    std::string name = Upper(Trim(value));
    const auto &aliases = Spec().at("aliases");
    if (aliases.contains(name)) name = aliases.at(name).get<std::string>();
    if (!Spec().at("functions").contains(name)) throw std::runtime_error("Unsupported function: " + name);
    return name;
}
std::string FormatInput(const std::string &formatter, const json &v) {
    if (formatter == "function") return Function(Text(v));
    if (formatter == "borough") {
        if (!Truth(v)) return "";
        std::string value = Upper(Trim(Text(v)));
        if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) return value;
        if (Spec().at("boroughs").contains(value)) return Text(Spec().at("boroughs").at(value));
        throw std::runtime_error(value + " is not a valid borough");
    }
    static const std::map<std::string, std::pair<std::string, std::string>> flags = {
        {"auxseg", {"Y", "N"}}, {"cross_street_names", {"E", ""}},
        {"long_work_area_2", {"L", ""}}, {"mode_switch", {"X", ""}},
        {"real_streets_only", {"R", ""}}, {"roadbed_request_switch", {"R", ""}},
        {"street_name_normalization", {"C", ""}}, {"tpad", {"Y", "N"}}
    };
    auto f = flags.find(formatter);
    if (f != flags.end()) {
        if (v.is_boolean()) return v.get<bool>() ? f->second.first : f->second.second;
        return Truth(v) ? Upper(Trim(Text(v))).substr(0, 1) : f->second.second;
    }
    auto value = Text(v);
    if (formatter == "CT") { std::replace(value.begin(), value.end(), ' ', '0'); return value; }
    return Upper(Trim(value));
}
std::string Slice(const std::string &wa, const json &field) {
    size_t begin = field.at("i").at(0), end = field.at("i").at(1);
    return begin < wa.size() ? wa.substr(begin, end - begin) : "";
}
json ParseArea(const json &layout, const std::string &wa);
json List(const std::string &text, size_t width, const std::string &layout = "", bool nodes = false) {
    json out = json::array();
    for (size_t offset = 0; offset < text.size(); offset += width) {
        auto value = text.substr(offset, width);
        if (Trim(value).empty()) break;
        if (nodes) out.push_back(List(value, 32, "node_items"));
        else if (layout == "node_items") out.push_back(List(value, 8));
        else if (!layout.empty()) out.push_back(ParseArea(Spec().at("layouts").at("output").at(layout), value));
        else out.push_back(Trim(value));
    }
    return out;
}
json ParseField(const json &field, const std::string &wa) {
    auto v = Slice(wa, field);
    const std::string formatter = field.at("formatter");
    if (formatter == "LGI") return List(v, 53, "LGI");
    if (formatter == "LGI-extended") return List(v, 116, "LGI-extended");
    if (formatter == "BINs") return List(v, 7, "BINs");
    if (formatter == "BINs-tpad") return List(v, 8, "BINs-tpad");
    if (formatter == "intersections") return List(v, 55, "INTERSECTION");
    if (formatter == "node_list") return List(v, 160, "", true);
    if (!formatter.empty() && formatter.find_first_not_of("0123456789") == std::string::npos)
        return List(v, std::stoul(formatter));
    return FormatInput(formatter, v);
}
json ParseArea(const json &layout, const std::string &wa) {
    json output = json::object();
    for (auto it = layout.begin(); it != layout.end(); ++it) {
        if (it.value().contains("i")) output[it.key()] = ParseField(it.value(), wa);
        else {
            output[it.key()] = json::object();
            for (auto child = it.value().begin(); child != it.value().end(); ++child)
                output[it.key()][child.key()] = ParseField(child.value(), wa);
        }
    }
    return output;
}
json Common(const json &raw, const std::string &name) {
    if (!raw.contains(name)) return nullptr;
    auto v = raw.at(name);
    if (v.is_object()) v = v.value(name, json(nullptr));
    return v == "" ? json(nullptr) : v;
}
json Summarize(const json &raw, const json &manifest) {
    auto bin = Common(raw, "Building Identification Number (BIN) of Input Address or NAP");
    if (bin.is_null()) bin = Common(raw, "Building Identification Number (BIN)");
    auto number = [&](const char *key) -> json {
        auto v = Common(raw, key);
        return v.is_null() ? v : json(std::stod(v.get<std::string>()));
    };
    return {{"bbl", Common(raw, "BOROUGH BLOCK LOT (BBL)")}, {"bin", bin},
        {"latitude", number("Latitude")}, {"longitude", number("Longitude")},
        {"grc", Common(raw, "Geosupport Return Code (GRC)")}, {"grc2", Common(raw, "Geosupport Return Code 2 (GRC 2)")},
        {"message", Common(raw, "Message")}, {"message2", Common(raw, "Message 2")},
        {"release", manifest.at("identity").at("release")}, {"engine_version", manifest.at("identity").at("engine_version")},
        {"bundle_id", manifest.at("bundle_id")}, {"raw", Canonical(raw)}};
}
} // namespace

struct Engine::Impl {
    void *library = nullptr;
    // NYC's shipped geo.h declares void geo(char *, char *).
    using Geo = void (*)(char *, char *);
    Geo geo = nullptr;
    json manifest;
    struct Entry { json result; size_t bytes; std::list<std::string>::iterator position; };
    std::map<std::string, Entry> cache;
    std::list<std::string> lru;
    size_t cache_bytes = 0;
    ~Impl() { if (library) dlclose(library); }
};

Engine::Engine(const fs::path &bundle) : impl(new Impl) {
#if !defined(__linux__) || !defined(__x86_64__)
    (void)bundle;
    throw std::runtime_error("Local Geosupport requires Linux x86-64; select the documented Docker runtime on macOS");
#else
    impl->manifest = ReadJSON(bundle / "manifest.json");
    impl->library = dlopen((bundle / "payload/lib/libgeo.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!impl->library) throw std::runtime_error(std::string("Cannot load Geosupport: ") + dlerror());
    dlerror();
    impl->geo = reinterpret_cast<Impl::Geo>(dlsym(impl->library, "geo"));
    if (const char *error = dlerror()) throw std::runtime_error(std::string("Missing geo C API: ") + error);
#endif
}
Engine::~Engine() = default;

json Engine::Call(const json &request) {
    auto function = Function(request.at("function"));
    const auto &params = request.at("params");
    if (!params.is_object() || params.contains("function"))
        throw std::runtime_error("Inputs must be an object; specify function separately");
    const std::string mode = request.value("mode", "regular");
    if (mode != "regular" && mode != "extended" && mode != "long" && mode != "tpad" && mode != "long+tpad" && mode != "extended+tpad")
        throw std::runtime_error("Unsupported mode: " + mode);
    auto key = Canonical(json::array({function, params, mode}));
    auto found = impl->cache.find(key);
    if (found != impl->cache.end()) {
        impl->lru.splice(impl->lru.end(), impl->lru, found->second.position);
        return found->second.result;
    }
    const auto &input = Spec().at("layouts").at("input").at("WA1");
    std::array<char, 1200> wa1;
    std::array<char, 32767> wa2{};
    wa1.fill(' ');
    auto put = [&](const std::string &name, const json &value) {
        if (!input.contains(name)) throw std::runtime_error("Unknown input field: " + name);
        if (!value.is_null() && !value.is_string() && !value.is_number_integer() && !value.is_boolean())
            throw std::runtime_error("Input " + name + " must be a string, integer, boolean or null");
        auto field = input.at(name);
        std::string formatted = FormatInput(field.at("formatter"), value);
        for (unsigned char c : formatted) if (c == 0 || c > 127)
            throw std::runtime_error("Input " + name + " must be ASCII without NUL characters");
        size_t begin = field.at("i").at(0), end = field.at("i").at(1);
        if (formatted.size() > end - begin) throw std::runtime_error("Input " + name + " exceeds Geosupport field width");
        std::fill(wa1.begin() + begin, wa1.begin() + end, ' ');
        std::copy(formatted.begin(), formatted.end(), wa1.begin() + begin);
    };
    for (auto it = params.begin(); it != params.end(); ++it) put(it.key(), it.value());
    wa1.fill(' ');
    // Preserve Python dict update precedence, including overlapping field aliases.
    nlohmann::ordered_json values = nlohmann::ordered_json::parse(params.dump());
    values["function"] = function;
    if (mode == "extended" || mode == "extended+tpad") values["mode_switch"] = true;
    if (mode.find("long") != std::string::npos) values["long_work_area_2"] = true;
    if (mode.find("tpad") != std::string::npos) values["tpad"] = true;
    values["work_area_format"] = "C";
    for (auto it = values.begin(); it != values.end(); ++it) put(it.key(), json(it.value()));
    const std::string initial(wa1.data(), wa1.size());
    auto flag = [&](const char *name, const char *value) { return ParseField(input.at(name), initial) == value; };
    bool extended = flag("mode_switch", "X"), is_long = flag("long_work_area_2", "L"), tpad = flag("tpad", "Y");
    bool auxseg = flag("auxseg", "Y");
    std::string actual_function = Function(Slice(initial, input.at("function")));
    std::string actual_mode = extended ? "extended" : is_long ? (tpad ? "long+tpad" : "long") : "regular";
    auto length = Spec().at("functions").at(actual_function).at(actual_mode);
    size_t count = length.is_null() ? 0 : length.get<size_t>() + (auxseg ? 500 : 0);
    if (count >= wa2.size()) throw std::runtime_error("Unsupported work area size");
    std::fill_n(wa2.begin(), count, ' ');
    impl->geo(wa1.data(), wa2.data());
    const std::string out1(wa1.data(), strnlen(wa1.data(), wa1.size()));
    const std::string out2(wa2.data(), strnlen(wa2.data(), wa2.size()));
    const auto &layouts = Spec().at("layouts").at("output");
    auto raw = ParseArea(layouts.at("WA1"), out1);
    if (layouts.contains(actual_function)) raw.update(ParseArea(layouts.at(actual_function), out2));
    if (layouts.contains(actual_function + "-" + actual_mode)) raw.update(ParseArea(layouts.at(actual_function + "-" + actual_mode), out2));
    if (auxseg) raw.update(ParseArea(layouts.at("AUXSEG"), out2.substr(out2.size() > 500 ? out2.size() - 500 : 0)));
    auto result = Summarize(raw, impl->manifest);
    size_t bytes = key.size() + Canonical(result).size();
    if (bytes <= 32 * 1024 * 1024) {
        impl->lru.push_back(key);
        impl->cache.emplace(key, Impl::Entry{result, bytes, std::prev(impl->lru.end())});
        impl->cache_bytes += bytes;
        while (impl->cache.size() > 10000 || impl->cache_bytes > 32 * 1024 * 1024) {
            auto oldest = impl->cache.find(impl->lru.front());
            impl->cache_bytes -= oldest->second.bytes;
            impl->cache.erase(oldest);
            impl->lru.pop_front();
        }
    }
    return result;
}
json Engine::Batch(const json &requests) {
    if (!requests.is_array()) throw std::runtime_error("Worker batch must be an array");
    json result = json::array();
    for (const auto &request : requests) result.push_back(Call(request));
    return result;
}

int EngineMain(const fs::path &bundle) {
    // Keep native library diagnostics off the JSON protocol channel.
    int protocol = dup(STDOUT_FILENO);
    if (protocol < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) throw std::runtime_error("Cannot isolate engine output");
    auto send = [&](const json &data) {
        auto bytes = Canonical(data) + "\n";
        if (bytes.size() > MAX_FRAME) throw std::runtime_error("Worker response exceeds protocol limit");
        size_t offset = 0;
        while (offset < bytes.size()) {
            auto count = write(protocol, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Worker protocol disconnected");
            offset += static_cast<size_t>(count);
        }
    };
    try {
        Engine engine(bundle);
        send({{"ready", ReadJSON(bundle / "manifest.json").at("bundle_id")}});
        std::string line;
        while (true) {
            line.clear();
            int c;
            while ((c = std::cin.get()) != EOF && c != '\n') {
                if (line.size() >= MAX_FRAME) throw std::runtime_error("Worker request exceeds protocol limit");
                line.push_back(static_cast<char>(c));
            }
            if (c == EOF) { if (!line.empty()) throw std::runtime_error("Incomplete worker frame"); break; }
            try { send({{"results", engine.Batch(json::parse(line))}}); }
            catch (const std::exception &e) { send({{"error", e.what()}}); }
        }
    } catch (const std::exception &e) { send({{"error", e.what()}}); close(protocol); return 1; }
    close(protocol);
    return 0;
}
} // namespace gs
