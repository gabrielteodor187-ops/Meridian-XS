#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void print_banner() {
    std::cout
        << "Meridian-XS\n"
        << "Xbox Series X|S research emulator project\n"
        << "Version 0.1.0 (development scaffold)\n\n";
}

void print_status() {
    std::cout
        << "Status: bootstrap only\n"
        << "CPU emulation: not implemented\n"
        << "GPU emulation: not implemented\n"
        << "System services: not implemented\n"
        << "Game compatibility: none yet\n\n"
        << "This executable verifies that the project builds and starts.\n";
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
