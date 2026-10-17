#include "native.hpp"
#include <iostream>
#include <signal.h>

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    try {
        if (argc == 3 && std::string(argv[1]) == "--engine") return gs::EngineMain(argv[2]);
        std::cerr << "Usage: geosupport-worker --engine /absolute/bundle\n";
        return 2;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
