#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
void print_banner() {
    std::cout
        << "Meridian-XS\n"
        << "Xbox Series X|S research emulator project\n"
        << "Version 0.3.0 (early CPU execution)\n\n";
}

void print_status() {
    std::cout
        << "Implemented foundations:\n"
        << "  - bounded guest RAM and little-endian access\n"
        << "  - x86-64 CPU architectural state\n"
        << "  - initial interpreter: NOP, HLT, MOV reg, immediate\n"
        << "  - core tests registered with CTest\n\n"
        << "Still missing:\n"
        << "  - broad x86-64 instruction coverage and flags\n"
        << "  - virtual memory, interrupts, and scheduling\n"
        << "  - Xbox kernel and system services\n"
        << "  - GPU emulation, storage, and boot support\n";
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
