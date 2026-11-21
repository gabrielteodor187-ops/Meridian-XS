#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
void print_banner() {
    std::cout
        << "Meridian-XS\n"
        << "Xbox Series X|S research emulator project\n"
        << "Version 0.2.0 (core foundations)\n\n";
}

void print_status() {
    std::cout
        << "Implemented foundations:\n"
        << "  - bounded guest RAM with little-endian typed access\n"
        << "  - x86-64 CPU architectural state container\n"
        << "  - core unit tests registered with CTest\n\n"
        << "Not implemented yet:\n"
        << "  - x86-64 instruction decoder/interpreter or JIT\n"
        << "  - Xbox system services and kernel interfaces\n"
        << "  - GPU command processor and graphics translation\n"
        << "  - storage, boot flow, and game compatibility\n";
}
} // namespace

int main(int argc, char** argv) {
    print_banner();
    if (argc > 1 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: meridian-xs [--help]\n";
        return EXIT_SUCCESS;
    }
    print_status();
    return EXIT_SUCCESS;
}
