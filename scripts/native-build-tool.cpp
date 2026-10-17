// Native replacement for resource generation and DuckDB's extension metadata script.
#include "nlohmann/json.hpp"
#include "picosha2/picosha2.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

std::string Read(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("Cannot read ") + path);
    return {std::istreambuf_iterator<char>(in), {}};
}
void Field(std::ostream &out, const std::string &value) {
    if (value.size() > 32) throw std::runtime_error("Metadata field exceeds 32 bytes");
    out << value << std::string(32 - value.size(), '\0');
}
int main(int argc, char **argv) {
    try {
        if (argc < 4) throw std::runtime_error("Usage: native-build-tool json|worker|metadata INPUT OUTPUT [SYMBOL|PLATFORM VERSION]");
        const std::string command = argv[1], bytes = Read(argv[2]);
        std::ofstream out(argv[3], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot create output");
        if (command == "json" && argc == 5) {
            auto compact = nlohmann::json::parse(bytes).dump(-1, ' ', true);
            out << "#pragma once\ninline constexpr char " << argv[4] << "[] = "
                << nlohmann::json(compact).dump(-1, ' ', true) << ";\n";
        } else if (command == "worker" && argc == 4) {
            out << "#pragma once\n#include <cstddef>\ninline constexpr unsigned char WORKER_BYTES[] = {\n";
            size_t i = 0;
            for (unsigned char byte : bytes) { out << unsigned(byte) << ','; if (++i % 32 == 0) out << '\n'; }
            out << "\n};\ninline constexpr size_t WORKER_SIZE = sizeof(WORKER_BYTES);\n"
                << "inline constexpr char WORKER_SHA256[] = \"" << picosha2::hash256_hex_string(bytes) << "\";\n";
        } else if (command == "metadata" && argc == 6) {
            if (!*argv[4] || !*argv[5]) throw std::runtime_error("Platform and extension version must not be empty");
            out << bytes;
            const unsigned char header[] = {0, 147, 4, 16};
            out.write(reinterpret_cast<const char *>(header), sizeof(header));
            out << "duckdb_signature";
            out.put(static_cast<char>(128)); out.put(4);
            const char *fields[] = {"", "", "", "C_STRUCT", argv[5], "v1.2.0", argv[4], "4"};
            for (const auto *field : fields) Field(out, field);
            out << std::string(256, '\0');
        } else throw std::runtime_error("Invalid arguments");
        out.close();
        if (!out) throw std::runtime_error("Cannot write output");
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
