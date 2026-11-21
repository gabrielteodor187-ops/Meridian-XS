#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
void print_banner() {
    std::cout
        << "Meridian-XS\n"
        << "Xbox Series X|S research emulator project\n"
        << "Version 0.94.0 (TEST memory operand support)\n\n";
}

void print_status() {
    std::cout
        << "Implemented foundations:\n"
        << "  - bounded guest RAM and little-endian access\n"
        << "  - x86-64 CPU architectural state\n"
        << "  - NOP, HLT, and MOV register-immediate\n"
        << "  - register-only 64-bit ADD, SUB, XOR, and CMP\n"
        << "  - basic arithmetic status flags\n"
        << "  - short JMP/JZ/JNZ and near JZ/JNZ branches\n"
        << "  - 64-bit PUSH/POP for general-purpose registers\n"
        << "  - near relative CALL and RET\n"
        << "  - 64-bit MOV loads/stores using base + disp8/disp32\n"
        << "  - 64-bit ADD/SUB/XOR/CMP with sign-extended imm32\n"
        << "  - SIB memory addressing with scaled and extended index registers\n"
        << "  - RIP-relative 64-bit MOV loads and stores with signed disp32\n"
        << "  - 32-bit register and immediate ADD/SUB/XOR/CMP with correct flags\n"
        << "  - 32-bit register-to-register MOV with zero-extension\n"
        << "  - core tests registered with CTest\n\n"
        << "Still missing:\n"
        << "  - broad x86-64 instruction coverage, including vector/SSE operations\n"
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
