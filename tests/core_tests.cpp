#include "meridian/core/guest_memory.hpp"
#include "meridian/core/virtual_address_space.hpp"
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
    const std::array<std::byte, 44> program{
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
        // CALL function at 0x6021; return address is 0x601b.
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
    require(state.rip == 0x601c, "HLT should advance RIP past the return site");
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

void test_memory_operand_mov() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x8000, 0x200};
    const std::array<std::byte, 31> program{
        // MOV [RDI+8], RAX; MOV RBX, [RDI+8]
        std::byte{0x48}, std::byte{0x89}, std::byte{0x47}, std::byte{0x08},
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x5f}, std::byte{0x08},
        // MOV [R8+16], RAX; MOV RBX, [R8+16]
        std::byte{0x49}, std::byte{0x89}, std::byte{0x40}, std::byte{0x10},
        std::byte{0x49}, std::byte{0x8b}, std::byte{0x58}, std::byte{0x10},
        // MOV [RDI+0x80], RAX; MOV RBX, [RDI+0x80] (disp32)
        std::byte{0x48}, std::byte{0x89}, std::byte{0x87}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x9f}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x8000, program), "memory-operand program should load");

    meridian::cpu::CpuState state{};
    state.rip = 0x8000;
    state.gpr[0] = 0x1122334455667788ULL;
    state.gpr[7] = 0x8040;
    state.gpr[8] = 0x8080;
    const meridian::cpu::Interpreter interpreter{};

    for (int i = 0; i < 6; ++i) {
        require(interpreter.step(state, memory) == StepResult::Executed,
                "register-indirect MOV should execute");
    }
    require(memory.read_le<std::uint64_t>(0x8048).value_or(0) == state.gpr[0],
            "MOV store should write through RDI plus disp8");
    require(memory.read_le<std::uint64_t>(0x8090).value_or(0) == state.gpr[0],
            "REX.B should select R8 as a memory base");
    require(memory.read_le<std::uint64_t>(0x80c0).value_or(0) == state.gpr[0],
            "MOV store should support a disp32 address");
    require(state.gpr[3] == state.gpr[0], "MOV loads should copy guest memory into RBX");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "memory-operand program should reach HLT");
}

void test_memory_operand_faults() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x9000, 0x100};
    const std::array<std::byte, 4> load{
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x5f}, std::byte{0x08}
    };
    require(memory.write(0x9000, load), "faulting load instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x9000;
    state.gpr[7] = 0x90f8;
    state.gpr[3] = 0xdeadbeef;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::MemoryFault,
            "out-of-range guest load should report a memory fault");
    require(state.rip == 0x9000 && state.gpr[3] == 0xdeadbeef,
            "failed guest load must not change RIP or its destination register");

}

void test_rip_relative_memory_addressing() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x9000, 0x200};
    const std::array<std::byte, 22> program{
        // MOV RBX, [RIP+0x39] -> 0x9040.
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x1d},
        std::byte{0x39}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // REX.B must be ignored for ModRM r/m=101 in 64-bit mode.
        std::byte{0x49}, std::byte{0x8b}, std::byte{0x0d},
        std::byte{0x32}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // MOV [RIP+0x3b] -> 0x9050, RAX.
        std::byte{0x48}, std::byte{0x89}, std::byte{0x05},
        std::byte{0x3b}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x9000, program), "RIP-relative program should load");
    require(memory.write_le<std::uint64_t>(0x9040, 0x1122334455667788ULL),
            "RIP-relative source data should be writable");
    meridian::cpu::CpuState state{};
    state.rip = 0x9000;
    state.gpr[0] = 0xaabbccddeeff0011ULL;
    state.gpr[13] = 0x9070;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative load should execute");
    require(state.gpr[3] == 0x1122334455667788ULL,
            "RIP-relative load should use the end of the instruction as its base");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative load with REX.B should execute");
    require(state.gpr[1] == 0x1122334455667788ULL,
            "REX.B must not turn RIP-relative addressing into an R13 base");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative store should execute");
    require(memory.read_le<std::uint64_t>(0x9050).value_or(0) == state.gpr[0],
            "RIP-relative store should write to the calculated target");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "RIP-relative program should reach HLT");

    // A negative displacement must address data before the instruction.
    const std::array<std::byte, 7> negative_load{
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x15},
        std::byte{0xd9}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}
    };
    require(memory.write(0x9060, negative_load), "negative RIP-relative load should load");
    state.rip = 0x9060;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "negative RIP-relative displacement should execute");
    require(state.gpr[2] == 0x1122334455667788ULL,
            "negative RIP-relative displacement should address earlier data");
}

void test_immediate_arithmetic() {
    using meridian::cpu::StepResult;
    constexpr std::uint64_t zero_flag = 1ULL << 6U;
    constexpr std::uint64_t overflow_flag = 1ULL << 11U;
    meridian::core::GuestMemory memory{0xa000, 64};
    const std::array<std::byte, 36> program{
        // ADD RAX, 5; SUB RAX, 2; XOR RAX, 0; CMP RAX, 6.
        std::byte{0x48}, std::byte{0x81}, std::byte{0xc0},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x81}, std::byte{0xe8},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x81}, std::byte{0xf0},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x81}, std::byte{0xf8},
        std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // ADD RAX, -1 (sign-extended imm32).
        std::byte{0x48}, std::byte{0x81}, std::byte{0xc0},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xf4}
    };
    require(memory.write(0xa000, program), "immediate arithmetic program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xa000;
    state.gpr[0] = 3;
    const meridian::cpu::Interpreter interpreter{};

    for (int i = 0; i < 4; ++i) {
        require(interpreter.step(state, memory) == StepResult::Executed,
                "supported immediate arithmetic should execute");
    }
    require(state.gpr[0] == 6, "ADD/SUB/XOR immediate operations should update RAX");
    require((state.rflags & zero_flag) != 0, "CMP immediate equality should set ZF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "negative imm32 should execute with sign extension");
    require(state.gpr[0] == 5, "negative imm32 should be sign-extended to 64 bits");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "immediate arithmetic program should reach HLT");

    meridian::core::GuestMemory overflow_memory{0xa100, 16};
    const std::array<std::byte, 7> overflow_program{
        std::byte{0x48}, std::byte{0x81}, std::byte{0xc0},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}
    };
    require(overflow_memory.write(0xa100, overflow_program),
            "overflow test instruction should load");
    meridian::cpu::CpuState overflow_state{};
    overflow_state.rip = 0xa100;
    overflow_state.gpr[0] = 0x7fffffffffffffffULL;
    require(interpreter.step(overflow_state, overflow_memory) == StepResult::Executed,
            "overflowing immediate ADD should execute");
    require(overflow_state.gpr[0] == 0x8000000000000000ULL,
            "immediate ADD should wrap at 64 bits");
    require((overflow_state.rflags & overflow_flag) != 0,
            "immediate ADD should set signed overflow");
}

void test_32bit_arithmetic() {
    using meridian::cpu::StepResult;
    constexpr std::uint64_t overflow_flag = 1ULL << 11U;
    constexpr std::uint64_t zero_flag = 1ULL << 6U;
    constexpr std::uint64_t sign_flag = 1ULL << 7U;
    meridian::core::GuestMemory memory{0xb000, 0x100};
    const std::array<std::byte, 38> program{
        // MOV EAX, 0x7fffffff; ADD EAX, 1 (overflow into sign bit).
        std::byte{0xb8}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f},
        std::byte{0x81}, std::byte{0xc0}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
        // SUB EAX, 0x80000000 -> zero.
        std::byte{0x81}, std::byte{0xe8}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x80},
        // MOV ECX, 5; ADD EAX, ECX; SUB EAX, ECX; XOR EAX, ECX; CMP EAX, ECX.
        std::byte{0xb9}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0xc8},
        std::byte{0x29}, std::byte{0xc8},
        std::byte{0x31}, std::byte{0xc8},
        std::byte{0x39}, std::byte{0xc8},
        std::byte{0xf4}
    };
    require(memory.write(0xb000, program), "32-bit arithmetic program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xb000;
    state.gpr[0] = 0xffffffffffffffffULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOV r32, imm32 should execute");
    require(state.gpr[0] == 0x7fffffffULL,
            "writing EAX must zero-extend into RAX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit immediate ADD should execute");
    require(state.gpr[0] == 0x80000000ULL,
            "32-bit ADD should wrap at 32 bits and zero-extend");
    require((state.rflags & overflow_flag) != 0 && (state.rflags & sign_flag) != 0,
            "32-bit ADD should set OF and SF at bit 31");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit immediate SUB should execute");
    require(state.gpr[0] == 0 && (state.rflags & zero_flag) != 0,
            "32-bit SUB should produce zero and set ZF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOV ECX, imm32 should execute");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit register ADD should execute");
    require(state.gpr[0] == 5, "32-bit register ADD should update EAX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit register SUB should execute");
    require(state.gpr[0] == 0, "32-bit register SUB should update EAX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit register XOR should execute");
    require(state.gpr[0] == 5, "32-bit register XOR should update EAX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit register CMP should execute");
    require(state.gpr[0] == 5, "32-bit CMP must not modify its destination");
    require((state.rflags & zero_flag) != 0,
            "32-bit CMP of equal operands should set ZF");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "32-bit arithmetic program should reach HLT");
}

void test_sib_memory_addressing() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x8000, 0x200};
    const std::array<std::byte, 33> program{
        // [RDI + RCX*4], using ordinary SIB indexing.
        std::byte{0x48}, std::byte{0x89}, std::byte{0x04}, std::byte{0x8f},
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x1c}, std::byte{0x8f},
        // [RDI + R8], with REX.X extending the SIB index.
        std::byte{0x4a}, std::byte{0x89}, std::byte{0x04}, std::byte{0x07},
        std::byte{0x4a}, std::byte{0x8b}, std::byte{0x1c}, std::byte{0x07},
        // [RCX*2 + 0x8000], SIB form with no base register.
        std::byte{0x48}, std::byte{0x89}, std::byte{0x04}, std::byte{0x4d},
        std::byte{0x00}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x1c}, std::byte{0x4d},
        std::byte{0x00}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x8000, program), "SIB program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x8000;
    state.gpr[0] = 0xaabbccddeeff0011ULL;
    state.gpr[1] = 2;
    state.gpr[7] = 0x8040;
    state.gpr[8] = 4;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "first SIB store should execute");
    require(memory.read_le<std::uint64_t>(0x8048).value_or(0) == state.gpr[0],
            "SIB scale and base/index registers should calculate the first address");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "first SIB load should execute");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "REX.X SIB store should execute");
    require(memory.read_le<std::uint64_t>(0x8044).value_or(0) == state.gpr[0],
            "REX.X should select R8 as the SIB index");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "REX.X SIB load should execute");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "no-base SIB store should execute");
    require(memory.read_le<std::uint64_t>(0x8004).value_or(0) == state.gpr[0],
            "SIB no-base form should use its signed disp32 and scaled index");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "no-base SIB load should execute");
    require(state.gpr[3] == state.gpr[0], "SIB loads should copy guest memory into RBX");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "SIB program should reach HLT");
}

void test_32bit_register_mov() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0xc000, 64};
    const std::array<std::byte, 13> program{
        // MOV ECX, EAX; MOV EDX, ECX.
        std::byte{0x89}, std::byte{0xc1},
        std::byte{0x8b}, std::byte{0xd1},
        // MOV R9D, R8D; MOV R10D, R9D (REX.R and REX.B).
        std::byte{0x45}, std::byte{0x89}, std::byte{0xc1},
        std::byte{0x45}, std::byte{0x8b}, std::byte{0xd1},
        std::byte{0xf4}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0xc000, program), "32-bit MOV program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xc000;
    state.gpr[0] = 0xffff000012345678ULL;
    state.gpr[1] = 0xaaaaaaaa00000000ULL;
    state.gpr[2] = 0xbbbbbbbb00000000ULL;
    state.gpr[8] = 0xcccccccc89abcdefULL;
    state.gpr[9] = 0xdddddddd00000000ULL;
    state.gpr[10] = 0xeeeeeeee00000000ULL;
    const meridian::cpu::Interpreter interpreter{};

    for (int i = 0; i < 4; ++i) {
        require(interpreter.step(state, memory) == StepResult::Executed,
                "32-bit register MOV should execute");
    }
    require(state.gpr[1] == 0x12345678ULL && state.gpr[2] == 0x12345678ULL,
            "32-bit MOV should copy values and zero-extend destinations");
    require(state.gpr[9] == 0x89abcdefULL && state.gpr[10] == 0x89abcdefULL,
            "REX.R and REX.B should select extended 32-bit registers");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "32-bit register MOV program should reach HLT");


}

void test_32bit_memory_mov() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0xd000, 0x100};
    const std::array<std::byte, 14> program{
        // MOV [RDI+4], EAX; MOV EBX, [RDI+4].
        std::byte{0x89}, std::byte{0x47}, std::byte{0x04},
        std::byte{0x8b}, std::byte{0x5f}, std::byte{0x04},
        // MOV [R15+8], EAX; MOV ECX, [R15+8] (REX.B).
        std::byte{0x41}, std::byte{0x89}, std::byte{0x47}, std::byte{0x08},
        std::byte{0x41}, std::byte{0x8b}, std::byte{0x4f}, std::byte{0x08}
    };
    require(memory.write(0xd000, program), "32-bit memory MOV program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xd000;
    state.gpr[0] = 0xffffeeee12345678ULL;
    state.gpr[7] = 0xd080;
    state.gpr[15] = 0xd090;
    const meridian::cpu::Interpreter interpreter{};

    for (int i = 0; i < 4; ++i) {
        require(interpreter.step(state, memory) == StepResult::Executed,
                "32-bit memory MOV should execute");
    }
    require(memory.read_le<std::uint32_t>(0xd084).value_or(0) == 0x12345678U,
            "32-bit memory store should write only the low four bytes");
    require(memory.read_le<std::uint32_t>(0xd098).value_or(0) == 0x12345678U,
            "REX.B should select the extended memory base register");
    require(state.gpr[3] == 0x12345678ULL && state.gpr[1] == 0x12345678ULL,
            "32-bit memory loads should zero-extend into destination GPRs");

    state.rip = 0xd0fc;
    const std::array<std::byte, 2> faulting_load{std::byte{0x8b}, std::byte{0x07}};
    require(memory.write(0xd0fc, faulting_load), "faulting load should be written");
    state.gpr[7] = 0xd0ff;
    require(interpreter.step(state, memory) == StepResult::MemoryFault,
            "32-bit load crossing the guest-memory boundary should fault");
    require(state.rip == 0xd0fc, "faulting 32-bit load must not advance RIP");
}

void test_test_instruction() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0xe000, 32};
    const std::array<std::byte, 9> program{
        // TEST EAX, ECX; TEST R8, R9; HLT.
        std::byte{0x85}, std::byte{0xc8},
        std::byte{0x4d}, std::byte{0x85}, std::byte{0xc8},
        std::byte{0xf4}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0xe000, program), "TEST program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xe000;
    state.gpr[0] = 0xffff0000000000f0ULL;
    state.gpr[1] = 0xaaaaaaaa0000000fULL;
    state.gpr[8] = 0x8000000000000000ULL;
    state.gpr[9] = 0x8000000000000000ULL;
    const auto original_rax = state.gpr[0];
    const auto original_rcx = state.gpr[1];
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit TEST should execute");
    require(state.gpr[0] == original_rax && state.gpr[1] == original_rcx,
            "TEST must not modify its operands");
    require((state.rflags & (1ULL << 6U)) != 0,
            "zero 32-bit AND result should set ZF");
    require((state.rflags & (1ULL << 0U)) == 0 &&
            (state.rflags & (1ULL << 11U)) == 0,
            "32-bit TEST should clear CF and OF");

    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit TEST with extended registers should execute");
    require((state.rflags & (1ULL << 7U)) != 0,
            "negative 64-bit AND result should set SF");
    require((state.rflags & (1ULL << 6U)) == 0,
            "nonzero 64-bit AND result should clear ZF");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "TEST program should reach HLT");
}

void test_memory_arithmetic() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0xf000, 0x100};
    const std::array<std::byte, 17> program{
        // ADD dword [RDI+4], ECX; XOR qword [RDI+8], R8; CMP dword [RDI+4], EDX.
        std::byte{0x01}, std::byte{0x4f}, std::byte{0x04},
        std::byte{0x4c}, std::byte{0x31}, std::byte{0x47}, std::byte{0x08},
        std::byte{0x39}, std::byte{0x57}, std::byte{0x04},
        std::byte{0x29}, std::byte{0x4f}, std::byte{0x04},
        std::byte{0xf4}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0xf000, program), "memory arithmetic program should load");
    require(memory.write_le<std::uint32_t>(0xf084, 10U), "initial dword should be written");
    require(memory.write_le<std::uint64_t>(0xf088, 0xff00ff00ff00ff00ULL),
            "initial qword should be written");
    meridian::cpu::CpuState state{};
    state.rip = 0xf000;
    state.gpr[7] = 0xf080;
    state.gpr[1] = 5;
    state.gpr[2] = 15;
    state.gpr[8] = 0x0f0f0f0f0f0f0f0fULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit memory ADD should execute");
    require(memory.read_le<std::uint32_t>(0xf084).value_or(0) == 15U,
            "32-bit memory ADD should store its result");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit memory XOR with R8 should execute");
    require(memory.read_le<std::uint64_t>(0xf088).value_or(0) ==
                (0xff00ff00ff00ff00ULL ^ 0x0f0f0f0f0f0f0f0fULL),
            "64-bit memory XOR should update the addressed qword");
    const auto before_cmp = memory.read_le<std::uint32_t>(0xf084).value_or(0);
    require(interpreter.step(state, memory) == StepResult::Executed,
            "memory CMP should execute");
    require(memory.read_le<std::uint32_t>(0xf084).value_or(0) == before_cmp,
            "memory CMP must not modify its operand");
    require((state.rflags & (1ULL << 6U)) != 0,
            "CMP 15 against 15 should set ZF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit memory SUB should execute");
    require(memory.read_le<std::uint32_t>(0xf084).value_or(0) == 10U,
            "32-bit memory SUB should store its result");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "memory arithmetic program should reach HLT");
}

void test_memory_arithmetic_extended_addressing() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x11000, 0x200};
    const std::array<std::byte, 18> program{
        // ADD qword [RDI + RCX*4 + 8], R8.
        std::byte{0x4c}, std::byte{0x01}, std::byte{0x44}, std::byte{0x8f}, std::byte{0x08},
        // XOR dword [RIP + disp32], EDX; displacement points to 0x11080.
        std::byte{0x31}, std::byte{0x15}, std::byte{0x75}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
        std::byte{0x90}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0x11000, program), "extended memory arithmetic program should load");
    require(memory.write_le<std::uint64_t>(0x11050, 10ULL),
            "SIB arithmetic target should initialize");
    require(memory.write_le<std::uint32_t>(0x11080, 0x00ff00ffU),
            "RIP-relative arithmetic target should initialize");
    meridian::cpu::CpuState state{};
    state.rip = 0x11000;
    state.gpr[7] = 0x11040;
    state.gpr[1] = 2;
    state.gpr[8] = 7;
    state.gpr[2] = 0x0f0f0f0fU;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "SIB memory ADD should execute");
    require(memory.read_le<std::uint64_t>(0x11050).value_or(0) == 17ULL,
            "SIB scale, base, displacement, and REX.R should select the right operands");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative memory XOR should execute");
    require(memory.read_le<std::uint32_t>(0x11080).value_or(0) ==
                (0x00ff00ffU ^ 0x0f0f0f0fU),
            "RIP-relative arithmetic should use the address after the instruction");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "extended memory arithmetic program should reach HLT");
}

void test_virtual_address_space() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    meridian::core::GuestMemory backing{0x20000, 3 * VirtualAddressSpace::page_size};
    VirtualAddressSpace address_space{};

    require(address_space.map(0x400000, backing, 0x20000,
                              2 * VirtualAddressSpace::page_size,
                              PagePermissions{true, true, true}),
            "two aligned pages should map");
    require(address_space.is_mapped(0x400ff0, 32),
            "range crossing two mapped pages should be mapped");
    require(!address_space.map(0x400000, backing, 0x22000,
                               VirtualAddressSpace::page_size,
                               PagePermissions{true, true, true}),
            "overlapping mappings must be rejected");
    require(!address_space.map(0x400001, backing, 0x22000,
                               VirtualAddressSpace::page_size,
                               PagePermissions{true, true, true}),
            "unaligned virtual addresses must be rejected");

    const std::array<std::byte, 32> payload{
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
        std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
        std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f},
        std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
        std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
        std::byte{0x28}, std::byte{0x29}, std::byte{0x2a}, std::byte{0x2b},
        std::byte{0x2c}, std::byte{0x2d}, std::byte{0x2e}, std::byte{0x2f}
    };
    require(address_space.write(0x400ff0, payload),
            "write spanning adjacent mapped pages should succeed");
    std::array<std::byte, 32> copied{};
    require(address_space.read(0x400ff0, copied) && copied == payload,
            "cross-page read should return the written bytes");
    std::array<std::byte, 32> fetched{};
    require(address_space.fetch(0x400ff0, fetched) && fetched == payload,
            "executable page fetch should succeed");

    require(address_space.unmap(0x401000, VirtualAddressSpace::page_size),
            "mapped page should unmap");
    require(!address_space.is_mapped(0x400ff0, 32),
            "range with an unmapped second page should no longer be mapped");
    require(!address_space.read(0x400ff0, copied),
            "cross-page read must fail when its second page is unmapped");
    require(!address_space.unmap(0x401000, VirtualAddressSpace::page_size),
            "unmapping a missing page should fail without changing mappings");

    require(address_space.map(0x401000, backing, 0x21000,
                              VirtualAddressSpace::page_size,
                              PagePermissions{true, false, false}),
            "read-only non-executable page should map");
    require(address_space.read(0x401000, std::span<std::byte>{copied.data(), 1}),
            "read permission should allow data reads");
    const std::array<std::byte, 1> replacement{std::byte{0xff}};
    const auto original = backing.read_le<std::uint8_t>(0x21000).value_or(0);
    require(!address_space.write(0x401000, replacement),
            "read-only page must reject writes");
    require(!address_space.fetch(0x401000, std::span<std::byte>{fetched.data(), 1}),
            "non-executable page must reject instruction fetches");
    require(backing.read_le<std::uint8_t>(0x21000).value_or(0) == original,
            "rejected write must not modify backing memory");

    require(address_space.map(0x402000, backing, 0x22000,
                              VirtualAddressSpace::page_size,
                              PagePermissions{false, true, false}),
            "write-only page should map");
    const std::array<std::byte, 1> marker{std::byte{0x5a}};
    require(address_space.write(0x402000, marker),
            "write-only page should allow writes");
    require(!address_space.read(0x402000, std::span<std::byte>{copied.data(), 1}),
            "write-only page must reject reads");

    require(!address_space.map(0x500000, backing, 0x20000,
                               4 * VirtualAddressSpace::page_size,
                               PagePermissions{true, true, true}),
            "mapping beyond backing memory must fail atomically");
    require(!address_space.is_mapped(0x500000),
            "failed mapping must not leave partial page entries");
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
        test_virtual_address_space();
        test_cpu_state_reset();
        test_interpreter();
        test_arithmetic_instructions();
        test_guest_program_end_to_end();
        test_branch_instructions();
        test_stack_and_call_instructions();
        test_stack_memory_fault_is_atomic();
        test_memory_operand_mov();
        test_memory_operand_faults();
        test_rip_relative_memory_addressing();
        test_immediate_arithmetic();
        test_32bit_arithmetic();
        test_sib_memory_addressing();
        test_32bit_register_mov();
        test_32bit_memory_mov();
        test_test_instruction();
        test_memory_arithmetic();
        test_memory_arithmetic_extended_addressing();
        std::cout << "All Meridian-XS core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
