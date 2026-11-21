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
        test_memory_operand_mov();
        test_memory_operand_faults();
        test_rip_relative_memory_addressing();
        test_immediate_arithmetic();
        test_32bit_arithmetic();
        test_sib_memory_addressing();
        std::cout << "All Meridian-XS core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
