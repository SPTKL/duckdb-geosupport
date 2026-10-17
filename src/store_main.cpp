#include "native.hpp"
#include <iostream>

// Development harness exercises the same Store implementation linked into DuckDB.
int main() {
    try {
        gs::json request;
        std::cin >> request;
        gs::Store store;
        std::string op = request.at("op");
        gs::json result;
        if (op == "install") result = store.Install(request.at("name"), request.value("archive", ""));
        else if (op == "verify") result = store.Verify(request.at("name"));
        else if (op == "resolve") { auto v = store.Resolve(request.at("name")); result = {{"path", v.first.string()}, {"manifest", v.second}}; }
        else if (op == "installed") result = store.Installed();
        else if (op == "extract") { gs::SafeExtract(request.at("archive").get<std::string>(), request.at("destination").get<std::string>()); result = nullptr; }
        else throw std::runtime_error("Unknown store operation");
        std::cout << gs::Canonical({{"result", result}}) << '\n';
    } catch (const std::exception &e) { std::cout << gs::Canonical({{"error", e.what()}}) << '\n'; return 1; }
}
