#include "meridian/core/guest_memory.hpp"
#include "meridian/cpu/cpu_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_guest_memory() {
    meridian::core::GuestMemory memory{0x1000, 64};
    require(memory.contains(0x1000, 64), "full guest memory range should be valid");
    require(!memory.contains(0x0fff), "address below base must be rejected");
    require(!memory.contains(0x1040), "one-past-end address must be rejected");
    require(memory.write_le<std::uint32_t>(0x1004, 0x12345678U),
            "write_le should accept an in-range value");
    const auto value = memory.read_le<std::uint32_t>(0x1004);
    require(value.has_value() && *value == 0x12345678U,
            "read_le should preserve little-endian values");
    require(!memory.read_le<std::uint32_t>(0x103e).has_value(),
            "out-of-range read must fail");
}

void test_cpu_state_reset() {
    meridian::cpu::CpuState state{};
    state.gpr[0] = 99;
    state.rip = 0xdeadbeef;
    state.rflags = 0xffff;
    state.xmm[3][2] = std::byte{0xff};
    state.reset();
    require(state.gpr[0] == 0 && state.rip == 0, "general CPU state should reset");
    require(state.rflags == 0x2, "RFLAGS reserved bit 1 should be set");
    require(state.xmm[3][2] == std::byte{0}, "vector registers should reset");
}
} // namespace

int main() {
    try {
        test_guest_memory();
        test_cpu_state_reset();
        std::cout << "All Meridian-XS core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
