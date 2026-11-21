#include "meridian/core/guest_memory.hpp"
#include "meridian/cpu/cpu_state.hpp"
#include "meridian/cpu/interpreter.hpp"

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

void test_interpreter() {
    meridian::core::GuestMemory memory{0x2000, 64};
    const std::array<std::byte, 12> program{
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0x88}, std::byte{0x77}, std::byte{0x66}, std::byte{0x55},
        std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},
        std::byte{0x90}, std::byte{0xf4}
    };
    require(memory.write(0x2000, program), "program should fit in guest memory");
    meridian::cpu::CpuState state{};
    state.rip = 0x2000;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::Executed,
            "MOV immediate should execute");
    require(state.gpr[0] == 0x1122334455667788ULL, "MOV should load the 64-bit immediate");
    require(state.rip == 0x200a, "MOV should advance RIP by ten bytes");
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::Executed,
            "NOP should execute");
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::Halted,
            "HLT should return the halted result");

    state.rip = 0x2040;
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::MemoryFault,
            "instruction fetch beyond mapped memory should fault");
    state.rip = 0x2000;
    require(memory.write_le<std::uint8_t>(0x2000, 0xCC), "test opcode should be writable");
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::UnsupportedInstruction,
            "unsupported opcodes should be reported without being executed");
}

void test_arithmetic_instructions() {
    using meridian::cpu::StepResult;
    constexpr std::uint64_t zero_flag = 1ULL << 6U;
    constexpr std::uint64_t carry_flag = 1ULL << 0U;
    constexpr std::uint64_t overflow_flag = 1ULL << 11U;

    meridian::core::GuestMemory memory{0x3000, 32};
    const std::array<std::byte, 16> program{
        std::byte{0x48}, std::byte{0x01}, std::byte{0xc8},
        std::byte{0x48}, std::byte{0x29}, std::byte{0xc8},
        std::byte{0x48}, std::byte{0x31}, std::byte{0xc8},
        std::byte{0x48}, std::byte{0x39}, std::byte{0xc8},
        std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0x3000, program), "arithmetic program should fit");
    meridian::cpu::CpuState state{};
    state.rip = 0x3000;
    state.gpr[0] = 5;
    state.gpr[1] = 3;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed, "ADD should execute");
    require(state.gpr[0] == 8, "ADD should store the sum");
    require(interpreter.step(state, memory) == StepResult::Executed, "SUB should execute");
    require(state.gpr[0] == 5, "SUB should store the difference");
    require(interpreter.step(state, memory) == StepResult::Executed, "XOR should execute");
    require(state.gpr[0] == 6, "XOR should store the bitwise result");
    state.gpr[1] = 6;
    require(interpreter.step(state, memory) == StepResult::Executed, "CMP should execute");
    require(state.gpr[0] == 6, "CMP must not modify its destination");
    require((state.rflags & zero_flag) != 0, "CMP of equal values should set zero flag");

    state.rip = 0x3000;
    state.gpr[0] = 0xffffffffffffffffULL;
    state.gpr[1] = 1;
    require(interpreter.step(state, memory) == StepResult::Executed, "overflowing ADD should execute");
    require(state.gpr[0] == 0, "64-bit ADD should wrap");
    require((state.rflags & carry_flag) != 0, "overflowing unsigned ADD should set carry");
    require((state.rflags & zero_flag) != 0, "zero result should set zero flag");

    state.rip = 0x3000;
    state.gpr[0] = 0x7fffffffffffffffULL;
    state.gpr[1] = 1;
    require(interpreter.step(state, memory) == StepResult::Executed, "signed-overflow ADD should execute");
    require((state.rflags & overflow_flag) != 0, "signed overflow should set overflow flag");
}

void test_guest_program_end_to_end() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x4000, 64};
    const std::array<std::byte, 23> program{
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0xb9},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x01}, std::byte{0xc8}
    };
    require(memory.write(0x4000, program), "guest program should load into memory");
    require(memory.write_le<std::uint8_t>(0x4017, 0xf4), "HLT should be written after program");

    meridian::cpu::CpuState state{};
    state.rip = 0x4000;
    const meridian::cpu::Interpreter interpreter{};

    std::size_t executed = 0;
    while (executed < 8) {
        const auto result = interpreter.step(state, memory);
        if (result == StepResult::Halted) break;
        require(result == StepResult::Executed, "guest program should execute without faults");
        ++executed;
    }

    require(executed == 3, "guest program should execute three instructions before HLT");
    require(state.gpr[0] == 12, "guest program should calculate 7 + 5 = 12");
    require(state.rip == 0x4018, "guest program should stop immediately after HLT");
}

void test_branch_instructions() {
    using meridian::cpu::StepResult;
    constexpr std::uint64_t zero_flag = 1ULL << 6U;
    meridian::core::GuestMemory memory{0x5000, 64};
    const std::array<std::byte, 17> program{
        std::byte{0x74}, std::byte{0x02},
        std::byte{0x90}, std::byte{0x90},
        std::byte{0xeb}, std::byte{0x02},
        std::byte{0x90}, std::byte{0x90},
        std::byte{0x75}, std::byte{0x02},
        std::byte{0x0f}, std::byte{0x84}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x5000, program), "branch program should fit");
    meridian::cpu::CpuState state{};
    state.rip = 0x5000;
    state.rflags |= zero_flag;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed && state.rip == 0x5004,
            "JZ should take a short branch when ZF is set");
    require(interpreter.step(state, memory) == StepResult::Executed && state.rip == 0x5008,
            "JMP should take an unconditional short branch");
    require(interpreter.step(state, memory) == StepResult::Executed && state.rip == 0x500a,
            "JNZ should fall through when ZF is set");
    require(interpreter.step(state, memory) == StepResult::Executed && state.rip == 0x5010,
            "near JZ should branch to the HLT instruction");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "branch target should reach HLT");
}

void test_stack_and_call_instructions() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x6000, 0x100};
    const std::array<std::byte, 43> program{
        // MOV RAX, 0x123456789abcdef0
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0xf0}, std::byte{0xde}, std::byte{0xbc}, std::byte{0x9a},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
        // PUSH RAX; MOV RAX, 0; POP RBX
        std::byte{0x50},
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x5b},
        // CALL function at 0x6020; return address is 0x601b.
        std::byte{0xe8}, std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4},
        std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
        // Function: MOV RCX, 7; RET.
        std::byte{0x48}, std::byte{0xb9},
        std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xc3}
    };
    require(memory.write(0x6000, program), "stack test program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x6000;
    state.gpr[4] = 0x6100;
    const meridian::cpu::Interpreter interpreter{};

    std::size_t executed = 0;
    while (executed < 12) {
        const auto result = interpreter.step(state, memory);
        if (result == StepResult::Halted) break;
        require(result == StepResult::Executed, "PUSH/POP/CALL/RET program should execute");
        ++executed;
    }

    require(executed == 7, "stack program should execute seven instructions before HLT");
    require(state.gpr[3] == 0x123456789abcdef0ULL, "POP RBX should restore the pushed RAX value");
    require(state.gpr[1] == 7, "called function should update RCX");
    require(state.gpr[4] == 0x6100, "PUSH/POP and CALL/RET should restore RSP");
    require(state.rip == 0x601b, "RET should return to the instruction after CALL before HLT");
}

void test_stack_memory_fault_is_atomic() {
    meridian::core::GuestMemory memory{0x7000, 32};
    require(memory.write_le<std::uint8_t>(0x7000, 0x50), "PUSH opcode should be writable");
    meridian::cpu::CpuState state{};
    state.rip = 0x7000;
    state.gpr[0] = 123;
    state.gpr[4] = 0x7000;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == meridian::cpu::StepResult::MemoryFault,
            "PUSH outside mapped memory should report a memory fault");
    require(state.rip == 0x7000 && state.gpr[4] == 0x7000,
            "failed PUSH must leave RIP and RSP unchanged");
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
        test_interpreter();
        test_arithmetic_instructions();
        test_guest_program_end_to_end();
        test_branch_instructions();
        test_stack_and_call_instructions();
        test_stack_memory_fault_is_atomic();
        std::cout << "All Meridian-XS core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
