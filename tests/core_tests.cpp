#include "meridian/core/guest_memory.hpp"
#include "meridian/core/virtual_address_space.hpp"
#include "meridian/core/x64_page_table_walker.hpp"
#include "meridian/core/pe_image_loader.hpp"
#include "meridian/core/x64_guest_image_mapper.hpp"
#include "meridian/core/xvd_container_reader.hpp"
#include "meridian/cpu/cpu_state.hpp"
#include "meridian/cpu/interpreter.hpp"
#include "meridian/cpu/execution_engine.hpp"

#include <array>
#include <bit>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
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

void test_virtual_address_protection_changes() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    meridian::core::GuestMemory backing{0x30000, 2 * VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(space.map(0x600000, backing, 0x30000,
                      2 * VirtualAddressSpace::page_size,
                      PagePermissions{true, true, false}),
            "two writable data pages should map");

    require(space.protect(0x600000, 2 * VirtualAddressSpace::page_size,
                          PagePermissions{true, false, true}),
            "protection should update a complete mapped range");
    std::array<std::byte, 1> byte{};
    const std::array<std::byte, 1> value{std::byte{0x77}};
    require(!space.write(0x600000, value),
            "write should fail after write permission is removed");
    require(space.fetch(0x600000, byte),
            "instruction fetch should succeed after execute permission is granted");

    require(!space.protect(0x600000, 3 * VirtualAddressSpace::page_size,
                           PagePermissions{true, true, true}),
            "protection across an unmapped page must fail");
    require(space.fetch(0x600000, byte),
            "failed multi-page protection must leave earlier pages unchanged");

    require(space.protect(0x600000, VirtualAddressSpace::page_size,
                          PagePermissions{false, true, false}),
            "a single page should be protectable independently");
    require(!space.read(0x600000, byte),
            "read should fail after read permission is removed");
    require(space.write(0x600000, value),
            "write should succeed after write permission is granted");
    require(!space.fetch(0x600000, byte),
            "execute should fail after execute permission is removed");
}


void test_virtual_address_typed_access() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    meridian::core::GuestMemory backing{0x50000, VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(space.map(0x700000, backing, 0x50000, VirtualAddressSpace::page_size,
                      PagePermissions{true, true, true}),
            "typed-access page should map");
    require(space.write_le<std::uint64_t>(0x700ff8, 0x1122334455667788ULL),
            "typed write should support a value crossing a page boundary only when mapped");
    const auto value = space.read_le<std::uint64_t>(0x700ff8);
    require(value && *value == 0x1122334455667788ULL,
            "typed read should preserve little-endian values");
    const auto instruction = space.fetch_le<std::uint64_t>(0x700ff8);
    require(instruction && *instruction == 0x1122334455667788ULL,
            "typed instruction fetch should honor execute permission");

    require(space.protect(0x700000, VirtualAddressSpace::page_size,
                          PagePermissions{true, true, false}),
            "typed-access page should become non-executable");
    require(!space.fetch_le<std::uint64_t>(0x700ff8),
            "typed fetch must reject non-executable pages");
    require(space.read_le<std::uint64_t>(0x700ff8).has_value(),
            "typed data read should remain available without execute permission");
}


void test_interpreter_virtual_address_space() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    using meridian::cpu::StepResult;

    meridian::core::GuestMemory backing{0x60000, 3 * VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(space.map(0x800000, backing, 0x60000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, true}),
            "CPU code page should map read/execute");
    require(space.map(0x900000, backing, 0x61000, VirtualAddressSpace::page_size,
                      PagePermissions{true, true, false}),
            "CPU data page should map read/write");
    const std::array<std::byte, 8> program{
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x07},
        std::byte{0x48}, std::byte{0x89}, std::byte{0x47}, std::byte{0x08},
        std::byte{0xf4}
    };
    require(backing.write(0x60000, program), "virtual-memory CPU program should load");
    require(space.write_le<std::uint64_t>(0x900000, 0x123456789abcdef0ULL),
            "virtual-memory CPU data should initialize");

    meridian::cpu::CpuState state{};
    state.rip = 0x800000;
    state.gpr[7] = 0x900000;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, space) == StepResult::Executed,
            "CPU should fetch code from an executable virtual page");
    require(state.gpr[0] == 0x123456789abcdef0ULL,
            "CPU data load should use readable virtual memory");
    require(interpreter.step(state, space) == StepResult::Executed,
            "CPU should store through writable virtual memory");
    require(space.read_le<std::uint64_t>(0x900008).value_or(0) ==
                0x123456789abcdef0ULL,
            "CPU virtual-memory store should update the mapped backing page");
    require(interpreter.step(state, space) == StepResult::Halted,
            "CPU should fetch HLT through the virtual address space");

    require(space.protect(0x800000, VirtualAddressSpace::page_size,
                          PagePermissions{true, false, false}),
            "code page should become non-executable");
    state.rip = 0x800000;
    require(interpreter.step(state, space) == StepResult::MemoryFault,
            "CPU instruction fetch must respect execute permission");
    require(state.rip == 0x800000,
            "instruction-fetch fault must leave RIP unchanged");

    require(space.protect(0x800000, VirtualAddressSpace::page_size,
                          PagePermissions{true, false, true}),
            "code page should become executable again");
    require(space.protect(0x900000, VirtualAddressSpace::page_size,
                          PagePermissions{true, false, false}),
            "data page should become read-only");
    state.rip = 0x800003;
    state.gpr[0] = 0xfeedfacecafebeefULL;
    require(interpreter.step(state, space) == StepResult::MemoryFault,
            "CPU data store must respect write permission");
    require(state.rip == 0x800003,
            "data-write fault must leave RIP unchanged");
    require(space.read_le<std::uint64_t>(0x900008).value_or(0) ==
                0x123456789abcdef0ULL,
            "rejected CPU store must not modify backing memory");
}


void test_lea_effective_address() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x12000, 0x100};
    const std::array<std::byte, 19> program{
        // LEA RAX, [RDI + RCX*4 + 8].
        std::byte{0x48}, std::byte{0x8d}, std::byte{0x44},
        std::byte{0x8f}, std::byte{0x08},
        // LEA R8D, [RIP + 0x10] (REX.R selects R8; result zero-extends).
        std::byte{0x44}, std::byte{0x8d}, std::byte{0x05},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // LEA R9, [R12 + R13*2 - 4] (REX.R/X/B).
        std::byte{0x4f}, std::byte{0x8d}, std::byte{0x4c},
        std::byte{0x6c}, std::byte{0xfc},
        std::byte{0xf4}, std::byte{0x90}
    };
    require(memory.write(0x12000, program), "LEA program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x12000;
    state.gpr[7] = 0x1000;
    state.gpr[1] = 3;
    state.gpr[8] = 0xaaaaaaaaaaaaaaaaULL;
    state.gpr[12] = 0x2000;
    state.gpr[13] = 5;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit LEA with SIB addressing should execute");
    require(state.gpr[0] == 0x1014,
            "LEA should calculate base plus scaled index plus displacement");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit RIP-relative LEA should execute");
    require(state.gpr[8] == 0x1201c,
            "32-bit LEA should calculate RIP-relative address and zero-extend");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "extended-register LEA should execute");
    require(state.gpr[9] == 0x2006,
            "REX.R/X/B should select extended LEA destination, index, and base");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "LEA test program should reach HLT");
}


void test_movsxd_registers() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x13000, 16};
    const std::array<std::byte, 10> program{
        // MOVSXD R9, R8D; MOVSXD R10, R9D; MOVSXD R11, R8D; HLT.
        std::byte{0x4d}, std::byte{0x63}, std::byte{0xc8},
        std::byte{0x4d}, std::byte{0x63}, std::byte{0xd1},
        std::byte{0x4d}, std::byte{0x63}, std::byte{0xd8},
        std::byte{0xf4}
    };
    require(memory.write(0x13000, program), "MOVSXD program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x13000;
    state.gpr[8] = 0x1234567880000001ULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSXD into R9 should execute");
    require(state.gpr[9] == 0xffffffff80000001ULL,
            "MOVSXD should sign-extend the source's low 32 bits");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSXD from R9 into R10 should execute");
    require(state.gpr[10] == 0xffffffff80000001ULL,
            "MOVSXD should preserve the signed 32-bit value");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSXD with REX.R/B extended registers should execute");
    require(state.gpr[11] == 0xffffffff80000001ULL,
            "REX.R and REX.B should select the extended destination and source");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "MOVSXD test program should reach HLT");
}


void test_movzx_registers() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x14000, 32};
    const std::array<std::byte, 17> program{
        // MOVZX EDX, AH; MOVZX RAX, CL; MOVZX R9, R8W; MOVZX EAX, SPL; HLT.
        std::byte{0x0f}, std::byte{0xb6}, std::byte{0xd4},
        std::byte{0x48}, std::byte{0x0f}, std::byte{0xb6}, std::byte{0xc1},
        std::byte{0x4d}, std::byte{0x0f}, std::byte{0xb7}, std::byte{0xc8},
        std::byte{0x40}, std::byte{0x0f}, std::byte{0xb6}, std::byte{0xc4},
        std::byte{0xf4}, std::byte{0x90}
    };
    require(memory.write(0x14000, program), "MOVZX program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x14000;
    state.gpr[1] = 0x123456789abc00abULL;
    state.gpr[8] = 0xfeedfacecafebeefULL;
    state.gpr[4] = 0x112233445566007aULL;
    state.gpr[0] = 0x000000000000ab00ULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVZX AH without a REX prefix should execute");
    require(state.gpr[2] == 0xab,
            "legacy byte register encoding should select AH without REX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVZX byte to 64-bit register should execute");
    require(state.gpr[0] == 0xab,
            "MOVZX byte source should zero-extend into a 64-bit destination");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVZX word with extended registers should execute");
    require(state.gpr[9] == 0xbeef,
            "REX.R/B should select extended registers for MOVZX word");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVZX SPL with a REX prefix should execute");
    require(state.gpr[0] == 0x7a,
            "REX prefix should select SPL rather than the legacy high-byte register");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "MOVZX test program should reach HLT");
}


void test_movsx_registers() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x15000, 16};
    const std::array<std::byte, 13> program{
        // MOVSX RAX, CL; MOVSX R9, R8W; MOVSX EAX, SPL; HLT.
        std::byte{0x48}, std::byte{0x0f}, std::byte{0xbe}, std::byte{0xc1},
        std::byte{0x4d}, std::byte{0x0f}, std::byte{0xbf}, std::byte{0xc8},
        std::byte{0x40}, std::byte{0x0f}, std::byte{0xbe}, std::byte{0xc4},
        std::byte{0xf4}
    };
    require(memory.write(0x15000, program), "MOVSX program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x15000;
    state.gpr[1] = 0x123456789abc0080ULL;
    state.gpr[8] = 0xfeedfacecafebeefULL;
    state.gpr[4] = 0x1122334455660080ULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSX byte to 64-bit register should execute");
    require(state.gpr[0] == 0xffffffffffffff80ULL,
            "MOVSX byte should sign-extend into a 64-bit destination");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSX word with extended registers should execute");
    require(state.gpr[9] == 0xffffffffffffbeefULL,
            "MOVSX word should sign-extend and honor REX.R/B");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSX SPL to 32-bit register should execute");
    require(state.gpr[0] == 0x00000000ffffff80ULL,
            "32-bit MOVSX should sign-extend to 32 bits then zero-extend the GPR");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "MOVSX test program should reach HLT");
}


void test_mov_immediate_group_c7() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x16000, 24};
    const std::array<std::byte, 14> program{
        // MOV RAX, -1 (sign-extended imm32); MOV R9D, 0x12345678; HLT.
        std::byte{0x48}, std::byte{0xc7}, std::byte{0xc0},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x41}, std::byte{0xc7}, std::byte{0xc1},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}
    };
    require(memory.write(0x16000, program), "C7 MOV program should load");
    const std::array<std::byte, 1> halt{std::byte{0xf4}};
    require(memory.write(0x1600e, halt), "C7 MOV program HLT should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x16000;
    state.gpr[0] = 0xaaaaaaaaaaaaaaaaULL;
    state.gpr[9] = 0xbbbbbbbbbbbbbbbbULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "C7 MOV to a 64-bit register should execute");
    require(state.gpr[0] == 0xffffffffffffffffULL,
            "64-bit C7 MOV should sign-extend its imm32");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "C7 MOV to an extended 32-bit register should execute");
    require(state.gpr[9] == 0x12345678ULL,
            "32-bit C7 MOV should zero-extend and honor REX.B");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "C7 MOV test program should reach HLT");
}


void test_mov_immediate_c7_memory() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x17000, 0x200};
    const std::array<std::byte, 17> program{
        // MOV qword [RDI+8], -1; MOV dword [RDI+RCX*4+4], 0x12345678; HLT.
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x47}, std::byte{0x08},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xc7}, std::byte{0x44}, std::byte{0x8f}, std::byte{0x04},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
        std::byte{0xf4}
    };
    require(memory.write(0x17000, program), "C7 memory MOV program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x17000;
    state.gpr[7] = 0x17100;
    state.gpr[1] = 2;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit C7 memory MOV should execute");
    require(memory.read_le<std::uint64_t>(0x17108).value_or(0) ==
                0xffffffffffffffffULL,
            "64-bit C7 memory MOV should sign-extend imm32");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "SIB-addressed 32-bit C7 memory MOV should execute");
    require(memory.read_le<std::uint32_t>(0x1710c).value_or(0) == 0x12345678U,
            "C7 memory MOV should support scaled-index addressing");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "C7 memory MOV program should reach HLT");

    state.rip = 0x17008;
    state.gpr[7] = 0x17200;
    require(interpreter.step(state, memory) == StepResult::MemoryFault,
            "C7 memory MOV outside guest RAM should fault");
    require(state.rip == 0x17008,
            "faulting C7 memory MOV must not advance RIP");
}


void test_mov_immediate_c7_rip_relative() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x18000, 0x100};
    const std::array<std::byte, 11> program{
        // MOV dword [RIP+0x36], 0x87654321; HLT.
        // RIP after the full instruction is 0x1800a, so the target is 0x18040.
        std::byte{0xc7}, std::byte{0x05},
        std::byte{0x36}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87},
        std::byte{0xf4}
    };
    require(memory.write(0x18000, program), "RIP-relative C7 program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x18000;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative C7 memory MOV should execute");
    require(memory.read_le<std::uint32_t>(0x18040).value_or(0) == 0x87654321U,
            "RIP-relative C7 should use the address after the complete instruction");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "RIP-relative C7 program should reach HLT");
}


void test_c7_memory_write_permissions_in_virtual_space() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory backing{0x70000, 2 * VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(space.map(0xa00000, backing, 0x70000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, true}),
            "C7 code page should map read/execute");
    require(space.map(0xb00000, backing, 0x71000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, false}),
            "C7 data page should map read-only");

    const std::array<std::byte, 6> program{
        // MOV dword [RDI], 0x12345678.
        std::byte{0xc7}, std::byte{0x07},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}
    };
    require(backing.write(0x70000, program), "C7 virtual-memory program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xa00000;
    state.gpr[7] = 0xb00000;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, space) == StepResult::MemoryFault,
            "C7 memory MOV must respect virtual-page write permissions");
    require(state.rip == 0xa00000,
            "rejected C7 virtual-memory store must leave RIP unchanged");
    require(backing.read_le<std::uint32_t>(0x71000).value_or(0) == 0,
            "rejected C7 store must not modify read-only backing memory");
}


void test_c7_cross_page_permission_fault_is_atomic() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory backing{0x73000, 3 * VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(space.map(0xa10000, backing, 0x73000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, true}),
            "C7 atomicity code page should map");
    require(space.map(0xb10000, backing, 0x74000, VirtualAddressSpace::page_size,
                      PagePermissions{true, true, false}),
            "first C7 data page should be writable");
    require(space.map(0xb11000, backing, 0x75000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, false}),
            "second C7 data page should be read-only");
    const std::array<std::byte, 7> program{
        // MOV qword [RDI], sign-extended 0x12345678; crosses the page boundary.
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x07},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}
    };
    require(backing.write(0x73000, program), "C7 atomicity program should load");
    require(backing.write_le<std::uint32_t>(0x74ffc, 0xaabbccddU),
            "first-page sentinel should initialize");
    require(backing.write_le<std::uint32_t>(0x75000, 0x55667788U),
            "second-page sentinel should initialize");

    meridian::cpu::CpuState state{};
    state.rip = 0xa10000;
    state.gpr[7] = 0xb10ffc;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, space) == StepResult::MemoryFault,
            "cross-page C7 store must fault when its second page is read-only");
    require(state.rip == 0xa10000,
            "cross-page C7 permission fault must leave RIP unchanged");
    require(backing.read_le<std::uint32_t>(0x74ffc).value_or(0) == 0xaabbccddU,
            "cross-page C7 fault must not partially modify the writable first page");
    require(backing.read_le<std::uint32_t>(0x75000).value_or(0) == 0x55667788U,
            "cross-page C7 fault must preserve the read-only second page");
}


void test_movsx_movzx_memory_operands() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1a000, 0x200};
    const std::array<std::byte, 17> program{
        // MOVZX RAX, byte [RDI+1]; MOVZX R9, word [RDI+RCX*4+2];
        // MOVSX RDX, byte [RDI+3]; HLT.
        std::byte{0x48}, std::byte{0x0f}, std::byte{0xb6}, std::byte{0x47}, std::byte{0x01},
        std::byte{0x4c}, std::byte{0x0f}, std::byte{0xb7}, std::byte{0x4c},
        std::byte{0x8f}, std::byte{0x02},
        std::byte{0x48}, std::byte{0x0f}, std::byte{0xbe}, std::byte{0x57}, std::byte{0x03},
        std::byte{0xf4}
    };
    require(memory.write(0x1a000, program), "memory MOVSX/MOVZX program should load");
    require(memory.write_le<std::uint8_t>(0x1a101, 0xab),
            "byte MOVZX source should initialize");
    require(memory.write_le<std::uint16_t>(0x1a106, 0xbeef),
            "word MOVZX source should initialize");
    require(memory.write_le<std::uint8_t>(0x1a103, 0x80),
            "byte MOVSX source should initialize");

    meridian::cpu::CpuState state{};
    state.rip = 0x1a000;
    state.gpr[7] = 0x1a100;
    state.gpr[1] = 1;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVZX byte from memory should execute");
    require(state.gpr[0] == 0xab,
            "MOVZX memory byte should zero-extend into RAX");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "SIB-addressed MOVZX word from memory should execute");
    require(state.gpr[9] == 0xbeef,
            "MOVZX memory word should honor scaled-index addressing and REX.R");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSX byte from memory should execute");
    require(state.gpr[2] == 0xffffffffffffff80ULL,
            "MOVSX memory byte should sign-extend into RDX");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "memory MOVSX/MOVZX program should reach HLT");
}


void test_movsxd_memory_operands() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1b000, 0x200};
    const std::array<std::byte, 10> program{
        // MOVSXD RAX, dword [RDI+4]; MOVSXD R9, dword [RDI+RCX*4+8]; HLT.
        std::byte{0x48}, std::byte{0x63}, std::byte{0x47}, std::byte{0x04},
        std::byte{0x4c}, std::byte{0x63}, std::byte{0x4c}, std::byte{0x8f},
        std::byte{0x08}, std::byte{0xf4}
    };
    require(memory.write(0x1b000, program), "memory MOVSXD program should load");
    require(memory.write_le<std::uint32_t>(0x1b104, 0x80000001U),
            "negative MOVSXD source should initialize");
    require(memory.write_le<std::uint32_t>(0x1b10c, 0x7fffffffU),
            "positive MOVSXD source should initialize");
    meridian::cpu::CpuState state{};
    state.rip = 0x1b000;
    state.gpr[7] = 0x1b100;
    state.gpr[1] = 1;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSXD from base-plus-displacement memory should execute");
    require(state.gpr[0] == 0xffffffff80000001ULL,
            "MOVSXD memory source should sign-extend a negative dword");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "SIB-addressed MOVSXD memory source should execute");
    require(state.gpr[9] == 0x7fffffffULL,
            "MOVSXD memory source should support SIB and extended destination registers");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "memory MOVSXD program should reach HLT");
}


void test_movzx_virtual_read_permissions() {
    using meridian::core::PagePermissions;
    using meridian::core::VirtualAddressSpace;
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory backing{0x76000, 2 * VirtualAddressSpace::page_size};
    VirtualAddressSpace space{};
    require(backing.write_le<std::uint8_t>(0x77000, 0x5a),
            "MOVZX permission-test data should initialize");
    require(space.map(0xc00000, backing, 0x76000, VirtualAddressSpace::page_size,
                      PagePermissions{true, false, true}),
            "MOVZX code page should map read/execute");
    require(space.map(0xd00000, backing, 0x77000, VirtualAddressSpace::page_size,
                      PagePermissions{false, true, false}),
            "MOVZX source page should map write-only");
    const std::array<std::byte, 4> program{
        // MOVZX RAX, byte [RDI].
        std::byte{0x48}, std::byte{0x0f}, std::byte{0xb6}, std::byte{0x07}
    };
    require(backing.write(0x76000, program), "MOVZX permission-test program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0xc00000;
    state.gpr[7] = 0xd00000;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, space) == StepResult::MemoryFault,
            "MOVZX memory source must respect virtual-page read permissions");
    require(state.rip == 0xc00000,
            "rejected MOVZX memory read must leave RIP unchanged");
}


void test_immediate_and_or_registers() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1c000, 32};
    const std::array<std::byte, 27> program{
        // OR EAX, 0x0f; AND EAX, 0xf0; OR RAX, -1; AND RAX, 0; HLT.
        std::byte{0x81}, std::byte{0xc8},
        std::byte{0x0f}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x81}, std::byte{0xe0},
        std::byte{0xf0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x48}, std::byte{0x81}, std::byte{0xc8},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x48}, std::byte{0x81}, std::byte{0xe0},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x1c000, program), "immediate AND/OR program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x1c000;
    state.gpr[0] = 0x12340000ULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit OR immediate should execute");
    require(state.gpr[0] == 0x1234000fULL,
            "32-bit OR immediate should update the low dword and zero-extend");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit AND immediate should execute");
    require(state.gpr[0] == 0,
            "32-bit AND immediate should clear bits outside the mask");
    require((state.rflags & (1ULL << 6U)) != 0,
            "zero AND result should set ZF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit OR immediate should execute");
    require(state.gpr[0] == 0xffffffffffffffffULL,
            "64-bit OR immediate should sign-extend imm32");
    require((state.rflags & ((1ULL << 0U) | (1ULL << 11U))) == 0,
            "logical OR should clear CF and OF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit AND immediate should execute");
    require(state.gpr[0] == 0,
            "64-bit AND immediate should update the full register");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "immediate AND/OR program should reach HLT");
}


void test_register_and_memory_or_and() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1d000, 0x200};
    const std::array<std::byte, 14> program{
        // OR EAX, ECX; AND RDX, RBX; OR dword [RDI], EAX;
        // AND qword [RDI+8], RCX; HLT.
        std::byte{0x09}, std::byte{0xc8},
        std::byte{0x48}, std::byte{0x21}, std::byte{0xda},
        std::byte{0x09}, std::byte{0x07},
        std::byte{0x48}, std::byte{0x21}, std::byte{0x4f}, std::byte{0x08},
        std::byte{0xf4}, std::byte{0x90}, std::byte{0x90}
    };
    require(memory.write(0x1d000, program), "register/memory OR/AND program should load");
    require(memory.write_le<std::uint32_t>(0x1d100, 0x0f0f0000U),
            "OR memory destination should initialize");
    require(memory.write_le<std::uint64_t>(0x1d108, 0xffff0000ffff0000ULL),
            "AND memory destination should initialize");

    meridian::cpu::CpuState state{};
    state.rip = 0x1d000;
    state.gpr[0] = 0x00f0000fU;
    state.gpr[1] = 0x0000f000U;
    state.gpr[2] = 0xffffffffffffffffULL;
    state.gpr[3] = 0x0000ffff0000ffffULL;
    state.gpr[7] = 0x1d100;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit register OR should execute");
    require(state.gpr[0] == 0x00f0f00fU,
            "32-bit register OR should produce the expected result");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit register AND should execute");
    require(state.gpr[2] == 0x0000ffff0000ffffULL,
            "64-bit register AND should produce the expected result");
    require((state.rflags & ((1ULL << 0U) | (1ULL << 11U))) == 0,
            "logical register operations should clear CF and OF");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit memory OR should execute");
    require(memory.read_le<std::uint32_t>(0x1d100).value_or(0) == 0x0ffff00fU,
            "32-bit memory OR should update the destination");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit memory AND should execute");
    require(memory.read_le<std::uint64_t>(0x1d108).value_or(0) ==
                0x0000000000000000ULL,
            "64-bit memory AND should update the destination");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "register/memory OR/AND program should reach HLT");
}


void test_sign_extended_imm8_group1() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1e000, 32};
    const std::array<std::byte, 12> program{
        // ADD EAX, -1; AND RAX, -128; OR R9, 0x7f; HLT.
        std::byte{0x83}, std::byte{0xc0}, std::byte{0xff},
        std::byte{0x48}, std::byte{0x83}, std::byte{0xe0}, std::byte{0x80},
        std::byte{0x49}, std::byte{0x83}, std::byte{0xc9}, std::byte{0x7f},
        std::byte{0xf4}
    };
    require(memory.write(0x1e000, program), "sign-extended imm8 program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x1e000;
    state.gpr[0] = 2;
    state.gpr[9] = 0x100;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "32-bit ADD with sign-extended imm8 should execute");
    require(state.gpr[0] == 1,
            "32-bit imm8 0xff should be interpreted as negative one");
    state.gpr[0] = 0x123456789abcdef0ULL;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit AND with sign-extended imm8 should execute");
    require(state.gpr[0] == 0x123456789abcde80ULL,
            "64-bit imm8 0x80 should sign-extend to negative 128");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "extended-register OR with imm8 should execute");
    require(state.gpr[9] == 0x17f,
            "REX.B should select R9 for immediate OR");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "sign-extended imm8 program should reach HLT");
}


void test_all_short_condition_codes() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x1f000, 32};
    const std::array<std::byte, 8> program{
        std::byte{0x70}, std::byte{0x05},
        std::byte{0xb8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0xf4}
    };
    require(memory.write(0x1f000, program), "condition-code test program should load");
    struct ConditionCase { std::uint8_t opcode; std::uint64_t flags; bool taken; };
    constexpr std::uint64_t cf = 1ULL << 0U;
    constexpr std::uint64_t pf = 1ULL << 2U;
    constexpr std::uint64_t zf = 1ULL << 6U;
    constexpr std::uint64_t sf = 1ULL << 7U;
    constexpr std::uint64_t of = 1ULL << 11U;
    const std::array<ConditionCase, 16> cases{{
        {0x70, of, true}, {0x71, of, false},
        {0x72, cf, true}, {0x73, cf, false},
        {0x74, zf, true}, {0x75, zf, false},
        {0x76, cf, true}, {0x77, cf, false},
        {0x78, sf, true}, {0x79, sf, false},
        {0x7a, pf, true}, {0x7b, pf, false},
        {0x7c, sf, true}, {0x7d, sf | of, true},
        {0x7e, zf, true}, {0x7f, 0, true}
    }};
    const meridian::cpu::Interpreter interpreter{};
    for (const auto& item : cases) {
        require(memory.write(0x1f001, std::span<const std::byte>{program}.subspan(1)),
                "condition-code branch body should remain available");
        const std::array<std::byte, 1> opcode_byte{static_cast<std::byte>(item.opcode)};
        require(memory.write(0x1f000, opcode_byte), "condition opcode should write");
        meridian::cpu::CpuState state{};
        state.rip = 0x1f000;
        state.rflags = 0x2 | item.flags;
        require(interpreter.step(state, memory) == StepResult::Executed,
                "short conditional branch should execute");
        const std::uint64_t expected_rip = 0x1f000 + (item.taken ? 7U : 2U);
        if (state.rip != expected_rip) {
            std::cerr << "Condition opcode " << static_cast<unsigned>(item.opcode)
                      << " expected RIP " << expected_rip
                      << " but got " << state.rip
                      << " with flags " << item.flags << '\n';
            require(false, "short conditional branch should evaluate its condition flags");
        }
    }
}

void test_near_unsigned_condition_branch() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x20000, 32};
    const std::array<std::byte, 12> program{
        // JB +5; MOV EAX,1; HLT.
        std::byte{0x0f}, std::byte{0x82},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xb8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0xf4}
    };
    require(memory.write(0x20000, program), "near conditional branch program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x20000;
    state.rflags = 0x2 | (1ULL << 0U);
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "near JB should execute");
    require(state.rip == 0x2000b,
            "near JB should branch when carry is set");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "near JB target should reach HLT");
}


void test_setcc_register_forms() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x21000, 32};
    const std::array<std::byte, 11> program{
        // SETZ AL; SETNZ AH; SETB R9B; HLT.
        std::byte{0x0f}, std::byte{0x94}, std::byte{0xc0},
        std::byte{0x0f}, std::byte{0x95}, std::byte{0xc4},
        std::byte{0x41}, std::byte{0x0f}, std::byte{0x92}, std::byte{0xc1},
        std::byte{0xf4}
    };
    require(memory.write(0x21000, program), "SETcc program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x21000;
    state.rflags = 0x2 | (1ULL << 6U) | (1ULL << 0U);
    state.gpr[0] = 0x1122334455665500ULL;
    state.gpr[9] = 0xabcdef00ULL;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "SETZ AL should execute");
    require((state.gpr[0] & 0xffU) == 1,
            "SETZ should write one to AL when ZF is set");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "SETNZ AH should execute");
    require(((state.gpr[0] >> 8U) & 0xffU) == 0,
            "SETNZ should clear AH when ZF is set");
    require((state.gpr[0] & ~0xffffULL) == 0x1122334455660000ULL,
            "SETcc byte writes should preserve the rest of the destination register");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "REX.B SETB R9B should execute");
    require((state.gpr[9] & 0xffU) == 1 &&
            (state.gpr[9] & ~0xffULL) == 0xabcdef00ULL,
            "REX.B SETcc should address an extended low-byte register");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "SETcc program should reach HLT");
}


void test_cmovcc_register_forms() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x22000, 32};
    const std::array<std::byte, 13> program{
        // CMOVZ RAX, RCX; CMOVNZ RBX, RDX; CMOVB R9, R10; HLT.
        std::byte{0x48}, std::byte{0x0f}, std::byte{0x44}, std::byte{0xc1},
        std::byte{0x48}, std::byte{0x0f}, std::byte{0x45}, std::byte{0xda},
        std::byte{0x4d}, std::byte{0x0f}, std::byte{0x42}, std::byte{0xca},
        std::byte{0xf4}
    };
    require(memory.write(0x22000, program), "CMOVcc program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x22000;
    state.rflags = 0x2 | (1ULL << 6U) | (1ULL << 0U);
    state.gpr[0] = 0x10;
    state.gpr[1] = 0x20;
    state.gpr[3] = 0x30;
    state.gpr[2] = 0x40;
    state.gpr[9] = 0x50;
    state.gpr[10] = 0x60;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "CMOVZ should execute when ZF is set");
    require(state.gpr[0] == 0x20,
            "CMOVZ should copy its source when the condition is true");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CMOVNZ should execute when ZF is set");
    require(state.gpr[3] == 0x30,
            "CMOVNZ should preserve its destination when the condition is false");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "REX-extended CMOVB should execute");
    require(state.gpr[9] == 0x60,
            "CMOVB should support REX.R/REX.B extended registers");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "CMOVcc program should reach HLT");
}


void test_setcc_memory_operands() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x23000, 0x100};
    const std::array<std::byte, 12> program{
        // SETZ byte [RDI+1]; SETNZ byte [RIP+0x15]; HLT.
        std::byte{0x0f}, std::byte{0x94}, std::byte{0x47}, std::byte{0x01},
        std::byte{0x0f}, std::byte{0x95}, std::byte{0x05},
        std::byte{0x15}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xf4}
    };
    require(memory.write(0x23000, program), "memory SETcc program should load");
    require(memory.write_le<std::uint8_t>(0x23011, 0xff),
            "base-displacement SETcc target should initialize");
    require(memory.write_le<std::uint8_t>(0x23020, 0xff),
            "RIP-relative SETcc target should initialize");
    meridian::cpu::CpuState state{};
    state.rip = 0x23000;
    state.gpr[7] = 0x23010;
    state.rflags = 0x2 | (1ULL << 6U);
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "SETZ memory destination should execute");
    require(memory.read_le<std::uint8_t>(0x23011).value_or(0xff) == 1,
            "SETZ should write one to its memory destination");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RIP-relative SETNZ memory destination should execute");
    require(memory.read_le<std::uint8_t>(0x23020).value_or(0xff) == 0,
            "SETNZ should write zero when its condition is false");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "memory SETcc program should reach HLT");
}


void test_group1_immediate_memory_destinations() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x24000, 0x100};
    const std::array<std::byte, 13> program{
        // ADD dword [RDI], 1; AND qword [RDI+8], -128;
        // CMP dword [RDI+4], 2; HLT.
        std::byte{0x83}, std::byte{0x07}, std::byte{0x01},
        std::byte{0x48}, std::byte{0x83}, std::byte{0x67},
        std::byte{0x08}, std::byte{0x80},
        std::byte{0x83}, std::byte{0x7f}, std::byte{0x04}, std::byte{0x02},
        std::byte{0xf4}
    };
    require(memory.write(0x24000, program), "Group-1 memory immediate program should load");
    require(memory.write_le<std::uint32_t>(0x24020, 5),
            "Group-1 ADD memory target should initialize");
    require(memory.write_le<std::uint32_t>(0x24024, 2),
            "Group-1 CMP memory target should initialize");
    require(memory.write_le<std::uint64_t>(0x24028, 0x1234),
            "Group-1 AND memory target should initialize");

    meridian::cpu::CpuState state{};
    state.rip = 0x24000;
    state.gpr[7] = 0x24020;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "imm8 ADD to memory should execute");
    require(memory.read_le<std::uint32_t>(0x24020).value_or(0) == 6,
            "imm8 ADD should update a 32-bit memory destination");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "64-bit imm8 AND to memory should execute");
    require(memory.read_le<std::uint64_t>(0x24028).value_or(0) == 0x1200,
            "64-bit memory AND should sign-extend its imm8 operand");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "imm8 CMP against memory should execute");
    require((state.rflags & (1ULL << 6U)) != 0,
            "memory CMP should set ZF for equal operands");
    require(memory.read_le<std::uint32_t>(0x24024).value_or(0) == 2,
            "memory CMP must not modify its destination");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "Group-1 memory immediate program should reach HLT");

    meridian::core::GuestMemory rip_memory{0x24100, 0x100};
    const std::array<std::byte, 8> rip_program{
        // ADD dword [RIP+0x16], 1; HLT.
        std::byte{0x83}, std::byte{0x05},
        std::byte{0x16}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0xf4}
    };
    require(rip_memory.write(0x24100, rip_program),
            "RIP-relative Group-1 program should load");
    require(rip_memory.write_le<std::uint32_t>(0x2411d, 9),
            "RIP-relative Group-1 target should initialize");
    meridian::cpu::CpuState rip_state{};
    rip_state.rip = 0x24100;
    require(interpreter.step(rip_state, rip_memory) == StepResult::Executed,
            "RIP-relative imm8 ADD to memory should execute");
    require(rip_memory.read_le<std::uint32_t>(0x2411d).value_or(0) == 10,
            "RIP-relative address should include the trailing immediate byte");
}


void test_x64_page_table_walker() {
    using meridian::core::GuestMemory;
    using meridian::core::X64MemoryAccess;
    using meridian::core::X64PageTableWalker;
    GuestMemory physical{0, 0x1000000};
    constexpr std::uint64_t present_rw_user = 0x7;
    require(physical.write_le<std::uint64_t>(0x1000, 0x2000 | present_rw_user),
            "PML4 entry should initialize");
    require(physical.write_le<std::uint64_t>(0x2000, 0x3000 | present_rw_user),
            "PDPT entry should initialize");
    require(physical.write_le<std::uint64_t>(0x3010, 0x4000 | present_rw_user),
            "PD entry should initialize");
    require(physical.write_le<std::uint64_t>(0x4000, 0x8000 | present_rw_user),
            "PT entry should initialize");

    const auto page = X64PageTableWalker::translate(
        physical, 0x1000, 0x400123, X64MemoryAccess::Read);
    require(page.has_value(), "four-level 4 KiB translation should succeed");
    require(page->physical_address == 0x8123 && page->page_size == 4096,
            "4 KiB translation should combine the frame and page offset");
    require(X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Write).has_value(),
            "writable page should allow writes");
    require(X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Execute).has_value(),
            "executable page should allow instruction fetches");
    require(X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Read, true).has_value(),
            "user access should succeed when every level is user-accessible");

    require(physical.write_le<std::uint64_t>(0x4000,
                0x8000 | present_rw_user | (1ULL << 63U)),
            "NX page entry should update");
    require(!X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Execute),
            "NX page should deny instruction fetches when NXE is enabled");
    require(X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Read).has_value(),
            "NX page should remain readable");

    require(physical.write_le<std::uint64_t>(0x4000, 0x8000 | present_rw_user),
            "writable page entry should restore");
    require(physical.write_le<std::uint64_t>(0x4000,
                0x8000 | (present_rw_user & ~0x2ULL)),
            "read-only page entry should update");
    require(!X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Write),
            "read-only page should deny supervisor writes when CR0.WP is enabled");
    require(X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Write,
                false, false).has_value(),
            "supervisor writes may bypass read-only pages when CR0.WP is disabled");
    require(!X64PageTableWalker::translate(
                physical, 0x1000, 0x400123, X64MemoryAccess::Write, true, false),
            "user writes must not bypass read-only page permissions");

    require(physical.write_le<std::uint64_t>(0x3010,
                0x600000 | present_rw_user | (1ULL << 7U)),
            "2 MiB large-page entry should initialize");
    const auto large = X64PageTableWalker::translate(
        physical, 0x1000, 0x400123, X64MemoryAccess::Read);
    require(large.has_value() && large->physical_address == 0x600123 &&
            large->page_size == (1ULL << 21U),
            "2 MiB page translation should preserve the large-page offset");

    require(!X64PageTableWalker::translate(
                physical, 0x1000, 0x0000800000000000ULL, X64MemoryAccess::Read),
            "non-canonical 48-bit virtual addresses should be rejected");
}


void test_interpreter_through_x64_page_tables() {
    using meridian::core::GuestMemory;
    using meridian::core::X64PagedMemory;
    using meridian::cpu::StepResult;
    GuestMemory physical{0, 0x10000};
    constexpr std::uint64_t present_rw = 0x3;
    require(physical.write_le<std::uint64_t>(0x1000, 0x2000 | present_rw),
            "paged CPU PML4 entry should initialize");
    require(physical.write_le<std::uint64_t>(0x2000, 0x3000 | present_rw),
            "paged CPU PDPT entry should initialize");
    require(physical.write_le<std::uint64_t>(0x3010, 0x4000 | present_rw),
            "paged CPU PD entry should initialize");
    require(physical.write_le<std::uint64_t>(0x4000, 0x8000 | present_rw),
            "paged CPU code PTE should initialize");
    require(physical.write_le<std::uint64_t>(0x4008, 0x9000 | present_rw),
            "paged CPU data PTE should initialize");

    const std::array<std::byte, 6> program{
        // MOV EAX, [RDI]; ADD EAX, 1; HLT.
        std::byte{0x8b}, std::byte{0x07},
        std::byte{0x83}, std::byte{0xc0}, std::byte{0x01},
        std::byte{0xf4}
    };
    require(physical.write(0x8000, program),
            "paged CPU program should load into physical RAM");
    require(physical.write_le<std::uint32_t>(0x9000, 41),
            "paged CPU data should initialize");

    X64PagedMemory memory{physical, 0x1000};
    meridian::cpu::CpuState state{};
    state.rip = 0x400000;
    state.gpr[7] = 0x401000;
    state.control_registers[0] = 1ULL << 16U; // CR0.WP
    state.control_registers[3] = 0x1000;      // CR3
    state.efer = 1ULL << 11U;                 // EFER.NXE
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPU should fetch and execute through four-level page tables");
    require(state.gpr[0] == 41,
            "paged CPU load should read from the translated physical data page");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPU arithmetic should execute through the paged backend");
    require(state.gpr[0] == 42,
            "paged CPU arithmetic should update the loaded value");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "paged CPU program should reach HLT");

    require(physical.write_le<std::uint64_t>(
                0x4000, 0x8000 | present_rw | (1ULL << 63U)),
            "paged CPU code PTE should become NX");
    memory.invalidate_page(0x400000);
    meridian::cpu::CpuState nx_state{};
    nx_state.rip = 0x400000;
    nx_state.control_registers[0] = 1ULL << 16U;
    nx_state.control_registers[3] = 0x1000;
    nx_state.efer = 1ULL << 11U;
    require(interpreter.step(nx_state, memory) == StepResult::MemoryFault,
            "CPU instruction fetch must honor page-table NX permissions");

    require(physical.write_le<std::uint64_t>(0x4000, 0x8000 | present_rw),
            "paged CPU code PTE should restore execute permission");
    memory.invalidate_page(0x400000);
    require(physical.write_le<std::uint64_t>(0x4008, 0x9000 | 0x1ULL),
            "paged CPU data PTE should become read-only");
    memory.invalidate_page(0x401000);
    const std::array<std::byte, 1> replacement{std::byte{0x2a}};
    require(!memory.write(0x401000, replacement),
            "paged CPU backend should reject writes to a read-only page");
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 41,
            "rejected paged write must preserve physical data");

    // TLB entries remain valid until the guest invalidates the page or changes CR3.
    require(physical.write_le<std::uint32_t>(0xa000, 99),
            "alternate physical data page should initialize");
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 41,
            "initial paged read should populate the TLB");
    require(physical.write_le<std::uint64_t>(0x4008, 0xa000 | present_rw),
            "data PTE should remap to an alternate physical page");
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 41,
            "cached translation should remain until explicit invalidation");
    memory.invalidate_page(0x401000);
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 99,
            "INVLPG-style invalidation should expose the new physical mapping");
    memory.set_context(0x1000, false, true, true);
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 99,
            "unchanged paging context should preserve valid TLB state");
}


void test_control_register_moves_and_invlpg() {
    using meridian::core::GuestMemory;
    using meridian::core::X64PagedMemory;
    using meridian::cpu::StepResult;
    GuestMemory physical{0, 0x10000};
    constexpr std::uint64_t present_rw_user = 0x7;
    require(physical.write_le<std::uint64_t>(0x1000, 0x2000 | present_rw_user),
            "CR test initial PML4 should initialize");
    require(physical.write_le<std::uint64_t>(0x5000, 0x2000 | present_rw_user),
            "CR test alternate PML4 should initialize");
    require(physical.write_le<std::uint64_t>(0x2000, 0x3000 | present_rw_user),
            "CR test PDPT should initialize");
    require(physical.write_le<std::uint64_t>(0x3010, 0x4000 | present_rw_user),
            "CR test PD should initialize");
    require(physical.write_le<std::uint64_t>(0x4000, 0x8000 | present_rw_user),
            "CR test code PTE should initialize");
    require(physical.write_le<std::uint64_t>(0x4008, 0x9000 | present_rw_user),
            "CR test data PTE should initialize");
    const std::array<std::byte, 17> cr_program{
        // MOV RAX, 0x5000; MOV CR3, RAX; MOV RBX, CR3; HLT.
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0x00}, std::byte{0x50}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x0f}, std::byte{0x22}, std::byte{0xd8},
        std::byte{0x0f}, std::byte{0x20}, std::byte{0xdb},
        std::byte{0xf4}
    };
    require(physical.write(0x8000, cr_program),
            "control-register test program should load");
    X64PagedMemory memory{physical, 0x1000};
    meridian::cpu::CpuState state{};
    state.rip = 0x400000;
    state.control_registers[0] = 1ULL << 16U;
    state.control_registers[3] = 0x1000;
    state.efer = 1ULL << 11U;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOV immediate before CR3 write should execute");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "privileged MOV CR3, RAX should execute at CPL0");
    require(state.control_registers[3] == 0x5000,
            "MOV to CR3 should update the architectural control register");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOV RBX, CR3 should execute using the new paging root");
    require(state.gpr[3] == 0x5000,
            "MOV from CR3 should copy the active paging root");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "control-register program should reach HLT");

    const std::array<std::byte, 4> invlpg_program{
        // INVLPG [RDI]; HLT.
        std::byte{0x0f}, std::byte{0x01}, std::byte{0x3f}, std::byte{0xf4}
    };
    require(physical.write(0x8000, invlpg_program),
            "INVLPG test program should load");
    require(physical.write_le<std::uint32_t>(0x9000, 41),
            "INVLPG old data page should initialize");
    require(physical.write_le<std::uint32_t>(0xa000, 99),
            "INVLPG replacement data page should initialize");
    require(physical.write_le<std::uint64_t>(0x4008, 0x9000 | present_rw_user),
            "INVLPG initial data PTE should initialize");
    memory.set_context(0x1000, false, true, true);
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 41,
            "INVLPG setup should populate a cached translation");
    require(physical.write_le<std::uint64_t>(0x4008, 0xa000 | present_rw_user),
            "INVLPG data PTE should remap");
    meridian::cpu::CpuState invlpg_state{};
    invlpg_state.rip = 0x400000;
    invlpg_state.gpr[7] = 0x401000;
    invlpg_state.control_registers[0] = 1ULL << 16U;
    invlpg_state.control_registers[3] = 0x1000;
    invlpg_state.efer = 1ULL << 11U;
    require(interpreter.step(invlpg_state, memory) == StepResult::Executed,
            "privileged INVLPG should execute");
    require(memory.read_le<std::uint32_t>(0x401000).value_or(0) == 99,
            "INVLPG should invalidate the stale virtual-page translation");
    require(interpreter.step(invlpg_state, memory) == StepResult::Halted,
            "INVLPG program should reach HLT");

    const std::array<std::byte, 3> privileged_program{
        std::byte{0x0f}, std::byte{0x20}, std::byte{0xd8}
    };
    require(physical.write(0x8000, privileged_program),
            "unprivileged CR access program should load");
    meridian::cpu::CpuState user_state{};
    user_state.rip = 0x400000;
    user_state.cpl = 3;
    user_state.control_registers[3] = 0x1000;
    user_state.efer = 1ULL << 11U;
    require(interpreter.step(user_state, memory) == StepResult::PrivilegeFault,
            "MOV from CR3 must fault outside ring zero");
}


void test_rdmsr_wrmsr_and_privilege() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x25000, 64};
    const std::array<std::byte, 20> program{
        // MOV ECX, IA32_EFER; MOV EAX, NXE; MOV EDX, 0; WRMSR; RDMSR; HLT.
        std::byte{0xb9}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc0},
        std::byte{0xb8}, std::byte{0x00}, std::byte{0x08}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xba}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x0f}, std::byte{0x30},
        std::byte{0x0f}, std::byte{0x32},
        std::byte{0xf4}
    };
    require(memory.write(0x25000, program), "RDMSR/WRMSR program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x25000;
    const meridian::cpu::Interpreter interpreter{};

    for (int i = 0; i < 5; ++i) {
        require(interpreter.step(state, memory) == StepResult::Executed,
                "modeled WRMSR sequence should execute");
    }
    require(state.efer == (1ULL << 11U),
            "WRMSR should update the modeled EFER value");
    require(state.gpr[0] == (1ULL << 11U) && state.gpr[2] == 0,
            "RDMSR should split the 64-bit MSR value across EDX:EAX");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "RDMSR/WRMSR program should reach HLT");

    meridian::core::GuestMemory invalid_memory{0x25100, 16};
    const std::array<std::byte, 7> invalid_msr{
        // MOV ECX, 0xffffffff; RDMSR.
        std::byte{0xb9}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0x0f}, std::byte{0x32}
    };
    require(invalid_memory.write(0x25100, invalid_msr),
            "unsupported MSR test program should load");
    meridian::cpu::CpuState invalid_state{};
    invalid_state.rip = 0x25100;
    require(interpreter.step(invalid_state, invalid_memory) == StepResult::Executed,
            "unsupported MSR index setup should execute");
    require(interpreter.step(invalid_state, invalid_memory) ==
                StepResult::GeneralProtectionFault,
            "unmodeled MSR accesses should raise a general-protection fault");

    const std::array<std::byte, 2> privileged{std::byte{0x0f}, std::byte{0x32}};
    require(invalid_memory.write(0x25108, privileged),
            "unprivileged RDMSR program should load");
    meridian::cpu::CpuState user_state{};
    user_state.rip = 0x25108;
    user_state.cpl = 3;
    require(interpreter.step(user_state, invalid_memory) == StepResult::PrivilegeFault,
            "RDMSR should be rejected outside ring zero");
}


void test_syscall_sysret_and_swapgs() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x26000, 0x200};
    const std::array<std::byte, 3> user_code{
        std::byte{0x0f}, std::byte{0x05}, std::byte{0xf4}
    };
    const std::array<std::byte, 2> kernel_code{
        std::byte{0x0f}, std::byte{0x07}
    };
    require(memory.write(0x26000, user_code), "SYSCALL user code should load");
    require(memory.write(0x26010, kernel_code), "SYSRET kernel code should load");

    meridian::cpu::CpuState state{};
    state.rip = 0x26000;
    state.cpl = 3;
    state.cs = 0x23;
    state.ss = 0x1b;
    state.efer = 1ULL; // EFER.SCE
    state.star = (0x10ULL << 48U) | (0x08ULL << 32U);
    state.lstar = 0x26010;
    state.sfmask = 1ULL; // Mask CF on entry.
    state.rflags = 0x2 | (1ULL << 9U) | 1ULL;
    const std::uint64_t original_flags = state.rflags;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "SYSCALL should enter the configured kernel target");
    require(state.rip == 0x26010 && state.cpl == 0,
            "SYSCALL should load LSTAR and enter CPL0");
    require(state.gpr[1] == 0x26002 && state.gpr[11] == original_flags,
            "SYSCALL should save return RIP in RCX and RFLAGS in R11");
    require((state.rflags & 1ULL) == 0,
            "SYSCALL should mask flags using SFMASK");
    require(state.cs == 0x08 && state.ss == 0x10,
            "SYSCALL should load the kernel code/data selectors");

    require(interpreter.step(state, memory) == StepResult::Executed,
            "SYSRET should return to the saved user context");
    require(state.rip == 0x26002 && state.cpl == 3,
            "SYSRET should restore the user RIP and CPL3");
    require(state.cs == 0x23 && state.ss == 0x1b,
            "SYSRET should construct the user selectors from STAR");
    require(state.rflags == original_flags,
            "SYSRET should restore user flags from R11");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "user return address should reach HLT");

    const std::array<std::byte, 4> swapgs_code{
        std::byte{0x0f}, std::byte{0x01}, std::byte{0xf8}, std::byte{0xf4}
    };
    require(memory.write(0x26100, swapgs_code), "SWAPGS code should load");
    meridian::cpu::CpuState kernel_state{};
    kernel_state.rip = 0x26100;
    kernel_state.cpl = 0;
    kernel_state.gs_base = 0x11110000;
    kernel_state.kernel_gs_base = 0x22220000;
    require(interpreter.step(kernel_state, memory) == StepResult::Executed,
            "SWAPGS should execute at CPL0");
    require(kernel_state.gs_base == 0x22220000 &&
            kernel_state.kernel_gs_base == 0x11110000,
            "SWAPGS should exchange GS_BASE and KERNEL_GS_BASE");

    meridian::cpu::CpuState user_swapgs{};
    user_swapgs.rip = 0x26100;
    user_swapgs.cpl = 3;
    require(interpreter.step(user_swapgs, memory) == StepResult::PrivilegeFault,
            "SWAPGS should be privileged");
}


void test_cpuid_rdtsc_and_rdtscp() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x27000, 64};
    const std::array<std::byte, 16> program{
        // CPUID leaf 0; CPUID leaf 1; RDTSC; RDTSCP; HLT.
        std::byte{0x0f}, std::byte{0xa2},
        std::byte{0xb8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00},
        std::byte{0x0f}, std::byte{0xa2},
        std::byte{0x0f}, std::byte{0x31},
        std::byte{0x0f}, std::byte{0x01}, std::byte{0xf9},
        std::byte{0xf4}, std::byte{0x90}
    };
    require(memory.write(0x27000, program), "CPUID/TSC program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x27000;
    state.gpr[0] = 0;
    state.tsc = 100;
    state.tsc_aux = 0x1234;
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPUID leaf 0 should execute");
    require(state.gpr[0] == 0x0d &&
            state.gpr[3] == 0x6972654dU &&
            state.gpr[2] == 0x6e616964U &&
            state.gpr[1] == 0x20205358U,
            "CPUID leaf 0 should return the Meridian-XS vendor and max leaf");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOV EAX,1 should execute");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPUID leaf 1 should execute");
    require((state.gpr[2] & ((1U << 4U) | (1U << 5U) | (1U << 15U))) ==
                ((1U << 4U) | (1U << 5U) | (1U << 15U)),
            "CPUID should advertise the implemented TSC/MSR/CMOV capabilities");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RDTSC should execute");
    require(state.gpr[0] == 100 && state.gpr[2] == 0,
            "RDTSC should return the current 64-bit guest TSC");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "RDTSCP should execute");
    require(state.gpr[0] == 101 && state.gpr[1] == 0x1234,
            "RDTSCP should return TSC and TSC_AUX");
    require(interpreter.step(state, memory) == StepResult::Halted,
            "CPUID/TSC program should reach HLT");

    const std::array<std::byte, 2> rdtsc{std::byte{0x0f}, std::byte{0x31}};
    require(memory.write(0x27020, rdtsc), "privileged RDTSC test should load");
    meridian::cpu::CpuState restricted{};
    restricted.rip = 0x27020;
    restricted.cpl = 3;
    restricted.control_registers[4] = 1ULL << 2U; // CR4.TSD
    require(interpreter.step(restricted, memory) == StepResult::GeneralProtectionFault,
            "CR4.TSD should restrict RDTSC outside CPL0");
}


void test_pe32_plus_image_loader() {
    using meridian::core::PeImageLoader;
    std::vector<std::byte> file(0x600);
    const auto put16 = [&](std::size_t offset, std::uint16_t value) {
        file[offset] = static_cast<std::byte>(value & 0xffU);
        file[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    };
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const auto put64 = [&](std::size_t offset, std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };

    constexpr std::size_t pe = 0x80;
    constexpr std::size_t opt = pe + 24;
    constexpr std::size_t sections = opt + 0xf0;
    constexpr std::uint64_t preferred_base = 0x140000000ULL;
    put16(0, 0x5a4d);
    put32(0x3c, pe);
    put32(pe, 0x00004550);
    put16(pe + 4, 0x8664);
    put16(pe + 6, 2);
    put16(pe + 20, 0xf0);
    put16(opt, 0x20b);
    put32(opt + 16, 0x1000);
    put64(opt + 24, preferred_base);
    put32(opt + 32, 0x1000);
    put32(opt + 36, 0x200);
    put32(opt + 56, 0x3000);
    put32(opt + 60, 0x200);
    put32(opt + 108, 16);
    put32(opt + 112 + 1 * 8, 0x2020);
    put32(opt + 112 + 1 * 8 + 4, 40);
    put32(opt + 112 + 5 * 8, 0x2000);
    put32(opt + 112 + 5 * 8 + 4, 12);

    const auto write_section = [&](std::size_t offset, const char* name,
                                   std::uint32_t virtual_size,
                                   std::uint32_t virtual_address,
                                   std::uint32_t raw_size,
                                   std::uint32_t raw_offset,
                                   std::uint32_t characteristics) {
        for (std::size_t i = 0; i < 8 && name[i] != '\0'; ++i)
            file[offset + i] = static_cast<std::byte>(name[i]);
        put32(offset + 8, virtual_size);
        put32(offset + 12, virtual_address);
        put32(offset + 16, raw_size);
        put32(offset + 20, raw_offset);
        put32(offset + 36, characteristics);
    };
    write_section(sections, ".text", 0x200, 0x1000, 0x200, 0x200, 0x60000020);
    write_section(sections + 40, ".reloc", 0x200, 0x2000, 0x200, 0x400, 0x42000040);

    file[0x200] = std::byte{0xb8}; // MOV EAX, 42; HLT.
    file[0x201] = std::byte{0x2a};
    file[0x202] = std::byte{0x00};
    file[0x203] = std::byte{0x00};
    file[0x204] = std::byte{0x00};
    file[0x205] = std::byte{0xf4};
    put64(0x210, preferred_base + 0x1234);
    put32(0x400, 0x1000); // Relocation page RVA.
    put32(0x404, 12);     // Block size.
    put16(0x408, 0xa010); // DIR64 at RVA 0x1010.
    put16(0x40a, 0x0000); // ABSOLUTE padding entry.
    put32(0x420, 0x2060); // OriginalFirstThunk.
    put32(0x42c, 0x2050); // DLL name RVA.
    put32(0x430, 0x2070); // FirstThunk.
    const char dll_name[] = "kernel32.dll";
    for (std::size_t i = 0; i < sizeof(dll_name); ++i)
        file[0x450 + i] = static_cast<std::byte>(dll_name[i]);
    put64(0x460, 0x2080); // Import-by-name RVA.
    put64(0x468, 0x8000000000000005ULL); // Import by ordinal 5.
    put64(0x470, 0); // OFT terminator / IAT placeholder.
    put16(0x480, 7); // Hint.
    const char function_name[] = "ExitProcess";
    for (std::size_t i = 0; i < sizeof(function_name); ++i)
        file[0x482 + i] = static_cast<std::byte>(function_name[i]);


    auto loaded = PeImageLoader::load(file, 0x150000000ULL);
    require(loaded.has_value(), "valid PE32+ image should load");
    require(loaded->imports.size() == 1 &&
            loaded->imports[0].name == "kernel32.dll" &&
            loaded->imports[0].symbols.size() == 2 &&
            loaded->imports[0].symbols[0].name == "ExitProcess" &&
            loaded->imports[0].symbols[0].hint == 7 &&
            !loaded->imports[0].symbols[0].by_ordinal &&
            loaded->imports[0].symbols[1].by_ordinal &&
            loaded->imports[0].symbols[1].ordinal == 5,
            "PE loader should parse named and ordinal import metadata");
    const auto unpatched_image = loaded->image;
    require(!PeImageLoader::resolve_imports(*loaded,
                [](const std::string&, const meridian::core::PeImportSymbol& symbol)
                    -> std::optional<std::uint64_t> {
                    if (!symbol.by_ordinal) return 0x7fff00001234ULL;
                    return std::nullopt;
                }),
            "missing import should make resolution fail");
    require(loaded->image == unpatched_image,
            "failed import resolution must not partially patch the IAT");
    require(PeImageLoader::resolve_imports(*loaded,
                [](const std::string& module,
                   const meridian::core::PeImportSymbol& symbol)
                    -> std::optional<std::uint64_t> {
                    if (module != "kernel32.dll") return std::nullopt;
                    return symbol.by_ordinal ? 0x7fff00005678ULL
                                             : 0x7fff00001234ULL;
                }),
            "complete resolver should bind all imported symbols");
    const auto image_u64 = [&](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(loaded->image[offset + i])) << (i * 8U);
        return value;
    };
    require(image_u64(0x2070) == 0x7fff00001234ULL &&
            image_u64(0x2078) == 0x7fff00005678ULL,
            "resolver should patch both named and ordinal IAT slots");

    require(loaded->image_base == 0x150000000ULL &&
            loaded->entry_point == 0x150001000ULL,
            "PE loader should apply the requested image base to the entry point");
    require(loaded->sections.size() == 2 &&
            loaded->sections[0].name == ".text" &&
            loaded->sections[1].name == ".reloc",
            "PE loader should preserve section metadata");
    std::uint64_t relocated = 0;
    for (std::size_t i = 0; i < 8; ++i)
        relocated |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(loaded->image[0x1010 + i])) << (i * 8U);
    require(relocated == 0x150001234ULL,
            "PE loader should apply DIR64 base relocations");
    require(loaded->image[0x1000] == std::byte{0xb8},
            "PE loader should map section contents at their RVAs");

    meridian::core::GuestMemory physical{0, 0x200000};
    meridian::core::X64GuestImageMapper mapper{physical};
    const auto mapped = mapper.map(*loaded, 4);
    require(mapped.has_value() && mapped->entry_point == loaded->entry_point &&
            mapped->pages_mapped >= 7,
            "guest image mapper should allocate image, page tables, and stack");
    meridian::core::X64PagedMemory paged{physical, mapped->cr3};
    meridian::cpu::CpuState cpu{};
    cpu.rip = mapped->entry_point;
    cpu.gpr[4] = mapped->stack_pointer;
    cpu.control_registers[0] = 1ULL << 16U;
    cpu.control_registers[3] = mapped->cr3;
    cpu.efer = 1ULL << 11U;
    const meridian::cpu::ExecutionEngine engine{};
    const auto execution = engine.run(cpu, paged, 16);
    require(execution.reason == meridian::cpu::RunStopReason::Halted &&
            execution.steps == 2,
            "mapped PE image should run to HLT through the bounded page-table execution engine");
    require(cpu.gpr[0] == 42,
            "mapped PE code should update the guest EAX register");
    require(paged.write_le<std::uint64_t>(mapped->stack_pointer - 8, 0xfeedfaceULL),
            "mapped initial stack should accept guest writes");
    require(paged.read_le<std::uint64_t>(mapped->stack_pointer - 8).value_or(0) ==
                0xfeedfaceULL,
            "mapped initial stack should return guest data");

    const auto preferred = PeImageLoader::load(file);
    require(preferred.has_value() && preferred->image_base == preferred_base,
            "PE loader should use the preferred base when no override is requested");
    require(!PeImageLoader::load(std::span<const std::byte>{file}.first(32)),
            "truncated DOS headers should be rejected");

    file[pe + 4] = std::byte{0x4c};
    file[pe + 5] = std::byte{0x01}; // Unsupported non-AMD64 machine.
    require(!PeImageLoader::load(file),
            "non-AMD64 PE images should be rejected");
}




void test_xvd_xvc_header_metadata_reader() {
    using meridian::core::XvdContainerReader;
    // Include enough backing bytes for the declared XVC metadata region.
    std::vector<std::byte> file(0xc000);
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const auto put64 = [&](std::size_t offset, std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i)
        file[0x200 + i] = static_cast<std::byte>(magic[i]);
    put32(0x208, 0); // Encryption and integrity are enabled.
    put32(0x20c, 3);
    put64(0x218, 0x100000);
    put32(0x280, 0); // Fixed volume.
    put32(0x284, 1); // Title content.
    put32(0x288, 0x2000);
    put32(0x28c, 0x1000);
    put32(0x290, 0x3000);
    put32(0x294, 0x1000);
    put32(0x298, 0xaa000);

    const auto info = XvdContainerReader::inspect(file);
    require(info.has_value(), "valid XVD header should be recognized");
    require(info->format_version == 3 && info->drive_size == 0x100000 &&
            info->type == 0 && info->content_type == 1,
            "XVD reader should expose core volume metadata");
    require(info->is_xvc && info->is_encrypted &&
            info->data_integrity_enabled,
            "title content should be recognized as XVC and report security flags");
    require(info->embedded_xvd_length == 0x2000 &&
            info->user_data_length == 0x1000 &&
            info->xvc_data_length == 0x3000 &&
            info->dynamic_header_length == 0x1000 &&
            info->block_size == 0xaa000,
            "XVD reader should expose declared metadata-region lengths");

    put32(0x208, (1U << 1U) | (1U << 2U));
    const auto unprotected = XvdContainerReader::inspect(file);
    require(unprotected.has_value() && !unprotected->is_encrypted &&
            !unprotected->data_integrity_enabled,
            "XVD security flags should be decoded without attempting decryption");
    require(!XvdContainerReader::inspect(
                std::span<const std::byte>{file}.first(0x2fff)),
            "truncated XVD headers should be rejected");
    file[0x200] = std::byte{0};
    require(!XvdContainerReader::inspect(file),
            "non-XVD magic should be rejected");
}


void test_xvc_descriptor_region_metadata() {
    using meridian::core::XvdContainerReader;
    std::vector<std::byte> file(0x5000);
    const auto put16 = [&](std::size_t offset, std::uint16_t value) {
        file[offset] = static_cast<std::byte>(value & 0xffU);
        file[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    };
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const auto put64 = [&](std::size_t offset, std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i)
            file[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i)
        file[0x200 + i] = static_cast<std::byte>(magic[i]);
    put32(0x208, 1U << 2U); // Disable integrity tree; leave encryption flag set.
    put32(0x20c, 3);
    put64(0x218, 0x1000);
    put32(0x280, 0);
    put32(0x284, 1);
    put32(0x290, 0x2000);
    put32(0x298, 0xaa000);

    constexpr std::size_t descriptor = 0x3000;
    put32(descriptor + 0xd10, 1);
    put32(descriptor + 0xd14, 1);
    put32(descriptor + 0xd24, 0x1234);
    put64(descriptor + 0xd28, 0x4000);
    constexpr std::size_t region = descriptor + 0xda8;
    put32(region, 0x1234);
    put16(region + 4, 0xffff);
    put32(region + 8, 0x10);
    put32(region + 12, 2);
    const char description[] = "XVC-MD";
    for (std::size_t i = 0; i < 6; ++i)
        put16(region + 0x10 + i * 2, static_cast<std::uint16_t>(description[i]));
    put64(region + 0x50, 0x8000);
    put64(region + 0x58, 0x2000);
    put64(region + 0x60, 0xabcdef);

    const auto info = XvdContainerReader::inspect(file);
    require(info.has_value(), "valid XVC descriptor should be parsed");
    require(info->xvc_info_offset == descriptor && info->xvc_info_version == 1,
            "XVC descriptor location and version should be derived from XVD metadata");
    require(info->initial_play_region_id == 0x1234 &&
            info->initial_play_offset == 0x4000,
            "XVC initial-play metadata should be exposed");
    require(info->regions.size() == 1 && info->regions[0].id == 0x1234 &&
            info->regions[0].key_id == 0xffff &&
            info->regions[0].description == "XVC-MD" &&
            info->regions[0].offset == 0x8000 &&
            info->regions[0].length == 0x2000,
            "XVC region metadata should be parsed without reading key material");

    put32(descriptor + 0xd14, 0xffffffffU);
    require(!XvdContainerReader::inspect(file),
            "impossible XVC region counts should be rejected");
}


void test_bounded_execution_engine() {
    using meridian::cpu::ExecutionEngine;
    using meridian::cpu::RunStopReason;
    meridian::core::GuestMemory memory{0x2a000, 64};
    const std::array<std::byte, 6> finite_program{
        std::byte{0xb8}, std::byte{0x2a}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xf4}
    };
    require(memory.write(0x2a000, finite_program),
            "bounded execution program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2a000;
    const ExecutionEngine engine{};
    const auto halted = engine.run(state, memory, 10);
    require(halted.reason == RunStopReason::Halted && halted.steps == 2,
            "execution engine should stop on HLT within the instruction budget");
    require(state.gpr[0] == 42,
            "execution engine should preserve guest instruction effects");

    const std::array<std::byte, 2> loop{
        std::byte{0xeb}, std::byte{0xfe}
    };
    require(memory.write(0x2a020, loop), "infinite-loop test should load");
    meridian::cpu::CpuState looping{};
    looping.rip = 0x2a020;
    const auto limited = engine.run(looping, memory, 5);
    require(limited.reason == RunStopReason::InstructionLimit &&
            limited.steps == 5 && looping.rip == 0x2a020,
            "execution engine should stop an infinite guest loop at its budget");
    const auto zero_budget = engine.run(looping, memory, 0);
    require(zero_budget.reason == RunStopReason::InstructionLimit &&
            zero_budget.steps == 0,
            "zero instruction budget should execute no guest instructions");
}


void test_sse_sse2_xmm_instructions() {
    using meridian::cpu::ExecutionEngine;
    using meridian::cpu::RunStopReason;
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x2b000, 0x200};
    const std::array<std::byte, 16> initial{
        std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0},
        std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0},
        std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0},
        std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0}, std::byte{0xf0}
    };
    const std::array<std::byte, 16> input{
        std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f},
        std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f},
        std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f},
        std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f}, std::byte{0x0f}
    };
    const std::array<std::byte, 14> program{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x6f}, std::byte{0xd0}, // MOVDQA XMM2,XMM0
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x6f}, std::byte{0x08}, // MOVDQU XMM1,[RAX]
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xef}, std::byte{0xc1}, // PXOR XMM0,XMM1
        std::byte{0xf4}, std::byte{0x90}
    };
    require(memory.write(0x2b000, program), "SSE2 program should load");
    require(memory.write(0x2b100, input), "SSE2 source vector should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2b000;
    state.gpr[0] = 0x2b100;
    state.control_registers[4] = 1ULL << 9U; // CR4.OSFXSR
    state.xmm[0] = initial;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVDQA register copy should execute");
    require(state.xmm[2] == initial,
            "MOVDQA should copy all 128 bits between XMM registers");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVDQU memory load should execute");
    require(state.xmm[1] == input,
            "MOVDQU should load 128 bits from an unaligned-capable memory operand");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PXOR should execute");
    for (const auto byte : state.xmm[0])
        require(byte == std::byte{0xff},
                "PXOR should XOR every byte lane in the XMM register");
    const std::array<std::byte, 5> store{
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x7f},
        std::byte{0x40}, std::byte{0x10}
    };
    require(memory.write(0x2b020, store), "MOVDQU store instruction should load");
    state.rip = 0x2b020;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVDQU memory store should execute");
    std::array<std::byte, 16> stored{};
    require(memory.read(0x2b110, stored) && stored == state.xmm[0],
            "MOVDQU should store all 128 XMM bits to guest memory");

    const std::array<std::byte, 4> aligned_load{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x6f}, std::byte{0x08}
    };
    require(memory.write(0x2b030, aligned_load),
            "MOVDQA alignment test instruction should load");
    meridian::cpu::CpuState misaligned{};
    misaligned.rip = 0x2b030;
    misaligned.gpr[0] = 0x2b101;
    misaligned.control_registers[4] = 1ULL << 9U;
    require(interpreter.step(misaligned, memory) == StepResult::GeneralProtectionFault,
            "MOVDQA should reject an unaligned memory operand");
    require(misaligned.rip == 0x2b030,
            "faulting MOVDQA must leave RIP unchanged");

    meridian::cpu::CpuState disabled{};
    disabled.rip = 0x2b000;
    disabled.gpr[0] = 0x2b100;
    disabled.xmm[0] = initial;
    require(interpreter.step(disabled, memory) == StepResult::UnsupportedInstruction,
            "SSE instructions should require CR4.OSFXSR in this initial model");

    const std::array<std::byte, 5> bitwise{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xdb},
        std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x2b040, bitwise), "PAND test instruction should load");
    meridian::cpu::CpuState and_state{};
    and_state.rip = 0x2b040;
    and_state.control_registers[4] = 1ULL << 9U;
    and_state.xmm[0] = initial;
    and_state.xmm[1] = input;
    require(interpreter.step(and_state, memory) == StepResult::Executed,
            "PAND should execute");
    for (const auto byte : and_state.xmm[0])
        require(byte == std::byte{0x00},
                "PAND should perform byte-wise bitwise AND");
}


void test_sse2_packed_integer_arithmetic() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x2c000, 64};
    const std::array<std::byte, 10> program{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xd4}, std::byte{0xc1}, // PADDQ XMM0,XMM1
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xfb}, std::byte{0xc2}, // PSUBQ XMM0,XMM2
        std::byte{0xf4}, std::byte{0x90}
    };
    require(memory.write(0x2c000, program),
            "packed integer arithmetic program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2c000;
    state.control_registers[4] = 1ULL << 9U;
    const auto put_qword = [&](std::size_t reg, std::size_t lane,
                               std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i)
            state.xmm[reg][lane * 8U + i] =
                static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const auto get_qword = [&](std::size_t reg, std::size_t lane) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(state.xmm[reg][lane * 8U + i])) << (i * 8U);
        return value;
    };
    put_qword(0, 0, 10);
    put_qword(0, 1, 20);
    put_qword(1, 0, 3);
    put_qword(1, 1, 4);
    put_qword(2, 0, 1);
    put_qword(2, 1, 2);
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PADDQ should execute");
    require(get_qword(0, 0) == 13 && get_qword(0, 1) == 24,
            "PADDQ should add both 64-bit lanes independently");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PSUBQ should execute");
    require(get_qword(0, 0) == 12 && get_qword(0, 1) == 22,
            "PSUBQ should subtract both 64-bit lanes independently");
}


void test_sse2_packed_compare_and_movemask() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x2d000, 64};
    const std::array<std::byte, 8> program{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x74}, std::byte{0xc1}, // PCMPEQB XMM0,XMM1
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xd7}, std::byte{0xd8}  // PMOVMSKB EBX,XMM0
    };
    require(memory.write(0x2d000, program), "compare/movemask program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2d000;
    state.control_registers[4] = 1ULL << 9U;
    for (std::size_t i = 0; i < 16; ++i) {
        state.xmm[0][i] = static_cast<std::byte>(i);
        state.xmm[1][i] = static_cast<std::byte>((i % 2U) == 0 ? i : i + 1U);
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PCMPEQB should execute");
    for (std::size_t i = 0; i < 16; ++i) {
        const std::byte expected = (i % 2U) == 0 ? std::byte{0xff} : std::byte{0x00};
        require(state.xmm[0][i] == expected,
                "PCMPEQB should produce all-ones or all-zero byte lanes");
    }
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PMOVMSKB should execute");
    require(state.gpr[3] == 0x5555U,
            "PMOVMSKB should collect the sign bits of all 16 XMM bytes");

    const std::array<std::byte, 4> greater_than{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x66}, std::byte{0xc1}
    };
    require(memory.write(0x2d020, greater_than),
            "PCMPGTD instruction should load");
    meridian::cpu::CpuState signed_state{};
    signed_state.rip = 0x2d020;
    signed_state.control_registers[4] = 1ULL << 9U;
    signed_state.xmm[0] = {
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}
    };
    signed_state.xmm[1] = {
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}
    };
    require(interpreter.step(signed_state, memory) == StepResult::Executed,
            "PCMPGTD should execute");
    require(signed_state.xmm[0][0] == std::byte{0xff} &&
            signed_state.xmm[0][4] == std::byte{0x00} &&
            signed_state.xmm[0][8] == std::byte{0x00} &&
            signed_state.xmm[0][12] == std::byte{0x00},
            "PCMPGTD should use signed 32-bit comparisons");
}


void test_sse2_pshufd_and_shufps() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x2e000, 0x100};
    const std::array<std::byte, 6> pshufd{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x70},
        std::byte{0xc1}, std::byte{0x1b}, std::byte{0xf4}
    };
    require(memory.write(0x2e000, pshufd), "PSHUFD program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2e000;
    state.control_registers[4] = 1ULL << 9U;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const std::uint32_t value = static_cast<std::uint32_t>(lane + 1U);
        for (std::size_t byte = 0; byte < 4; ++byte)
            state.xmm[1][lane * 4U + byte] =
                static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "PSHUFD should execute");
    const std::array<std::uint32_t, 4> expected{4, 3, 2, 1};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t actual = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            actual |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(state.xmm[0][lane * 4U + byte]))
                << (byte * 8U);
        require(actual == expected[lane],
                "PSHUFD should select dword lanes using its immediate control byte");
    }

    const std::array<std::byte, 5> shufps{
        std::byte{0x0f}, std::byte{0xc6}, std::byte{0xc1},
        std::byte{0xe4}, std::byte{0xf4}
    };
    require(memory.write(0x2e020, shufps), "SHUFPS program should load");
    meridian::cpu::CpuState float_shuffle{};
    float_shuffle.rip = 0x2e020;
    float_shuffle.control_registers[4] = 1ULL << 9U;
    float_shuffle.xmm[0] = state.xmm[1];
    float_shuffle.xmm[1] = state.xmm[1];
    require(interpreter.step(float_shuffle, memory) == StepResult::Executed,
            "SHUFPS should execute");
    require(float_shuffle.xmm[0] == state.xmm[1],
            "SHUFPS immediate 0xe4 should preserve the selected 0,1,2,3 lane order");
    const std::array<std::byte, 9> rip_relative_pshufd{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x70}, std::byte{0x05},
        std::byte{0x17}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x1b}
    };
    require(memory.write(0x2e040, rip_relative_pshufd),
            "RIP-relative PSHUFD instruction should load");
    std::array<std::byte, 16> shuffle_source{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const std::uint32_t value = static_cast<std::uint32_t>((lane + 1U) * 10U);
        for (std::size_t byte = 0; byte < 4; ++byte)
            shuffle_source[lane * 4U + byte] =
                static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
    require(memory.write(0x2e060, shuffle_source),
            "RIP-relative PSHUFD source should load");
    meridian::cpu::CpuState memory_shuffle{};
    memory_shuffle.rip = 0x2e040;
    memory_shuffle.control_registers[4] = 1ULL << 9U;
    require(interpreter.step(memory_shuffle, memory) == StepResult::Executed,
            "RIP-relative PSHUFD memory source should execute");
    const std::array<std::uint32_t, 4> memory_expected{40, 30, 20, 10};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t actual = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            actual |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(
                    memory_shuffle.xmm[0][lane * 4U + byte])) << (byte * 8U);
        require(actual == memory_expected[lane],
                "RIP-relative PSHUFD should read source data after accounting for imm8");
    }

}


void test_unsupported_legacy_prefix_combinations() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x2f000, 32};
    const std::array<std::byte, 7> prefixed_jcc{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x84},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}
    };
    require(memory.write(0x2f000, prefixed_jcc),
            "prefixed conditional branch test should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x2f000;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::UnsupportedInstruction,
            "unsupported operand-size-prefixed near Jcc must not execute as rel32");
    require(state.rip == 0x2f000,
            "unsupported prefixed instruction should leave RIP unchanged");
}


void test_fxsave_fxrstor_state_round_trip() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x30000, 0x1000};
    const std::array<std::byte, 3> fxsave{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x00}
    };
    const std::array<std::byte, 3> fxrstor{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x08}
    };
    require(memory.write(0x30000, fxsave), "FXSAVE instruction should load");
    require(memory.write(0x30010, fxrstor), "FXRSTOR instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x30000;
    state.gpr[0] = 0x30100;
    state.control_registers[4] = 1ULL << 9U;
    state.mxcsr = 0x1f80;
    state.x87_control_word = 0x027f;
    state.x87_status_word = 0x1234;
    state.x87_tag_word = 0x55;
    state.x87_opcode = 0x456;
    state.x87_instruction_pointer = 0x1122334455667788ULL;
    state.x87_data_pointer = 0x8877665544332211ULL;
    for (std::size_t i = 0; i < state.xmm[0].size(); ++i) {
        state.xmm[0][i] = static_cast<std::byte>(i + 1U);
        state.st[0][i] = static_cast<std::byte>(0xf0U + i);
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "FXSAVE should write the 512-byte state image");
    require(memory.read_le<std::uint32_t>(0x30118).value_or(0) == state.mxcsr,
            "FXSAVE should store MXCSR at the architectural offset");
    std::array<std::byte, 16> saved_xmm{};
    require(memory.read(0x301a0, saved_xmm) && saved_xmm == state.xmm[0],
            "FXSAVE should store XMM0 in the state image");

    state.rip = 0x30010;
    state.mxcsr = 0;
    state.x87_control_word = 0x037f;
    state.x87_status_word = 0;
    state.xmm[0].fill(std::byte{0});
    state.st[0].fill(std::byte{0});
    require(interpreter.step(state, memory) == StepResult::Executed,
            "FXRSTOR should restore the saved state image");
    require(state.mxcsr == 0x1f80 && state.x87_control_word == 0x027f &&
            state.x87_status_word == 0x1234 && state.x87_tag_word == 0x55 &&
            state.x87_opcode == 0x456,
            "FXRSTOR should restore modeled x87 environment and MXCSR fields");
    require(state.x87_instruction_pointer == 0x1122334455667788ULL &&
            state.x87_data_pointer == 0x8877665544332211ULL,
            "FXRSTOR should restore saved x87 instruction/data pointers");
    require(state.xmm[0][0] == std::byte{1} &&
            state.xmm[0][15] == std::byte{16} &&
            state.st[0][0] == std::byte{0xf0} &&
            state.st[0][15] == std::byte{0xff},
            "FXRSTOR should restore XMM and opaque x87 register storage");

    const std::array<std::byte, 3> misaligned_fxsave{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x00}
    };
    require(memory.write(0x30020, misaligned_fxsave),
            "misaligned FXSAVE instruction should load");
    state.rip = 0x30020;
    state.gpr[0] = 0x30101;
    require(interpreter.step(state, memory) == StepResult::GeneralProtectionFault,
            "FXSAVE should reject non-16-byte-aligned memory");

    state.rip = 0x30010;
    state.gpr[0] = 0x30100;
    require(memory.write_le<std::uint32_t>(0x30118, 0xffffffffU),
            "invalid MXCSR test value should be writable");
    const auto old_xmm = state.xmm;
    require(interpreter.step(state, memory) == StepResult::GeneralProtectionFault,
            "FXRSTOR should reject reserved MXCSR bits");
    require(state.xmm == old_xmm,
            "invalid FXRSTOR image should not partially change XMM state");
}


void test_movd_movq_xmm_transfers() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31000, 0x200};
    const std::array<std::byte, 18> program{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x6e}, std::byte{0xc0}, // MOVD XMM0,EAX
        std::byte{0x66}, std::byte{0x48}, std::byte{0x0f}, std::byte{0x6e},
        std::byte{0xc8}, // MOVQ XMM1,RAX
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x7e}, std::byte{0xc2}, // MOVD EDX,XMM0
        std::byte{0x66}, std::byte{0x48}, std::byte{0x0f}, std::byte{0x7e},
        std::byte{0xc9} // MOVQ RCX,XMM1
    };
    require(memory.write(0x31000, program), "MOVD/MOVQ program should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31000;
    state.gpr[0] = 0x1122334455667788ULL;
    state.control_registers[4] = 1ULL << 9U;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVD from EAX to XMM0 should execute");
    require(state.xmm[0][0] == std::byte{0x88} &&
            state.xmm[0][3] == std::byte{0x55} &&
            state.xmm[0][4] == std::byte{0x00},
            "MOVD should transfer 32 bits and zero the upper XMM bits");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVQ from RAX to XMM1 should execute");
    require(state.xmm[1][0] == std::byte{0x88} &&
            state.xmm[1][7] == std::byte{0x11} &&
            state.xmm[1][8] == std::byte{0x00},
            "MOVQ should transfer 64 bits and zero the upper XMM bits");
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.gpr[2] == 0x55667788ULL,
            "MOVD from XMM0 to EDX should zero-extend its 32-bit result");
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.gpr[1] == 0x1122334455667788ULL,
            "MOVQ from XMM1 to RCX should transfer all 64 bits");

    const std::array<std::byte, 4> memory_load{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x6e}, std::byte{0x08}
    };
    require(memory.write(0x31040, memory_load),
            "memory-source MOVD should load");
    require(memory.write_le<std::uint32_t>(0x31080, 0xaabbccddU),
            "MOVD memory source data should load");
    meridian::cpu::CpuState memory_state{};
    memory_state.rip = 0x31040;
    memory_state.gpr[0] = 0x31080;
    memory_state.control_registers[4] = 1ULL << 9U;
    require(interpreter.step(memory_state, memory) == StepResult::Executed,
            "MOVD from memory should execute");
    require(memory_state.xmm[1][0] == std::byte{0xdd} &&
            memory_state.xmm[1][3] == std::byte{0xaa} &&
            memory_state.xmm[1][4] == std::byte{0x00},
            "MOVD memory load should zero-extend into XMM");
}


void test_sse_packed_and_scalar_moves() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31100, 0x100};
    const std::array<std::byte, 24> program{
        std::byte{0x0f}, std::byte{0x28}, std::byte{0xc1}, // MOVAPS XMM0,XMM1
        std::byte{0x0f}, std::byte{0x10}, std::byte{0x08}, // MOVUPS XMM1,[RAX]
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x10}, std::byte{0x10}, // MOVSS XMM2,[RAX]
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x10}, std::byte{0xd9}, // MOVSS XMM3,XMM1
        std::byte{0xf2}, std::byte{0x0f}, std::byte{0x10}, std::byte{0x20}, // MOVSD XMM4,[RAX]
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x11}, std::byte{0x50},
        std::byte{0x10} // MOVSS [RAX+16],XMM2
    };
    require(memory.write(0x31100, program), "SSE move program should load");
    std::array<std::byte, 16> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<std::byte>(i + 1U);
    require(memory.write(0x31181, input), "unaligned SSE source should load");

    meridian::cpu::CpuState state{};
    state.rip = 0x31100;
    state.gpr[0] = 0x31181;
    state.control_registers[4] = 1ULL << 9U;
    state.xmm[1].fill(std::byte{0xaa});
    state.xmm[2].fill(std::byte{0xbb});
    state.xmm[3].fill(std::byte{0xcc});
    const meridian::cpu::Interpreter interpreter{};

    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.xmm[0] == state.xmm[1],
            "MOVAPS register form should copy a full XMM register");
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.xmm[1] == input,
            "MOVUPS should load an unaligned 128-bit memory operand");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSS memory load should execute");
    for (std::size_t i = 0; i < 4; ++i)
        require(state.xmm[2][i] == input[i],
                "MOVSS should load its low 32-bit scalar");
    for (std::size_t i = 4; i < 16; ++i)
        require(state.xmm[2][i] == std::byte{0},
                "MOVSS memory load should zero the upper XMM bits");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSS register form should execute");
    for (std::size_t i = 0; i < 4; ++i)
        require(state.xmm[3][i] == input[i],
                "MOVSS register form should copy the low scalar");
    for (std::size_t i = 4; i < 16; ++i)
        require(state.xmm[3][i] == std::byte{0xcc},
                "MOVSS register form should preserve upper destination bits");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSD memory load should execute");
    for (std::size_t i = 0; i < 8; ++i)
        require(state.xmm[4][i] == input[i],
                "MOVSD should load its low 64-bit scalar");
    for (std::size_t i = 8; i < 16; ++i)
        require(state.xmm[4][i] == std::byte{0},
                "MOVSD memory load should zero the upper 64 bits");
    require(interpreter.step(state, memory) == StepResult::Executed,
            "MOVSS memory store should execute");
    std::array<std::byte, 4> stored{};
    require(memory.read(0x31191, stored) &&
            stored == std::array<std::byte, 4>{
                input[0], input[1], input[2], input[3]},
            "MOVSS should write only the low 32 bits to memory");

    const std::array<std::byte, 3> aligned_load{
        std::byte{0x0f}, std::byte{0x28}, std::byte{0x08}
    };
    require(memory.write(0x31130, aligned_load),
            "MOVAPS alignment test should load");
    meridian::cpu::CpuState misaligned{};
    misaligned.rip = 0x31130;
    misaligned.gpr[0] = 0x31181;
    misaligned.control_registers[4] = 1ULL << 9U;
    require(interpreter.step(misaligned, memory) == StepResult::GeneralProtectionFault,
            "MOVAPS should reject unaligned memory operands");
}


void test_sse_floating_arithmetic_subset() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31200, 0x100};
    const std::array<std::byte, 4> addps{
        std::byte{0x0f}, std::byte{0x58}, std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x31200, addps), "ADDPS test instruction should load");
    meridian::cpu::CpuState packed{};
    packed.rip = 0x31200;
    packed.control_registers[4] = 1ULL << 9U;
    const std::array<float, 4> left{1.0F, 2.0F, 3.0F, 4.0F};
    const std::array<float, 4> right{10.0F, 20.0F, 30.0F, 40.0F};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const auto l = std::bit_cast<std::uint32_t>(left[lane]);
        const auto r = std::bit_cast<std::uint32_t>(right[lane]);
        for (std::size_t byte = 0; byte < 4; ++byte) {
            packed.xmm[0][lane * 4U + byte] =
                static_cast<std::byte>((l >> (byte * 8U)) & 0xffU);
            packed.xmm[1][lane * 4U + byte] =
                static_cast<std::byte>((r >> (byte * 8U)) & 0xffU);
        }
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(packed, memory) == StepResult::Executed,
            "ADDPS should execute");
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(packed.xmm[0][lane * 4U + byte]))
                << (byte * 8U);
        require(std::bit_cast<float>(bits) == left[lane] + right[lane],
                "ADDPS should add each packed single-precision lane");
    }

    const std::array<std::byte, 5> addss{
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x58},
        std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x31220, addss), "ADDSS test instruction should load");
    meridian::cpu::CpuState scalar{};
    scalar.rip = 0x31220;
    scalar.control_registers[4] = 1ULL << 9U;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const auto bits = std::bit_cast<std::uint32_t>(left[lane]);
        for (std::size_t byte = 0; byte < 4; ++byte)
            scalar.xmm[0][lane * 4U + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
    }
    scalar.xmm[1] = packed.xmm[1];
    const auto scalar_original = scalar.xmm[0];
    require(interpreter.step(scalar, memory) == StepResult::Executed,
            "ADDSS should execute");
    std::uint32_t scalar_bits = 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        scalar_bits |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(scalar.xmm[0][byte])) << (byte * 8U);
    require(std::bit_cast<float>(scalar_bits) == 11.0F,
            "ADDSS should add only the low single-precision lane");
    require(std::equal(scalar.xmm[0].begin() + 4, scalar.xmm[0].end(),
                       scalar_original.begin() + 4),
            "ADDSS should preserve the upper destination lanes");

    const std::array<std::byte, 4> addpd{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x58}, std::byte{0xc1}
    };
    require(memory.write(0x31240, addpd), "ADDPD test instruction should load");
    meridian::cpu::CpuState doubles{};
    doubles.rip = 0x31240;
    doubles.control_registers[4] = 1ULL << 9U;
    const std::array<double, 2> dleft{1.5, 2.5};
    const std::array<double, 2> dright{2.5, 3.5};
    for (std::size_t lane = 0; lane < 2; ++lane) {
        const auto l = std::bit_cast<std::uint64_t>(dleft[lane]);
        const auto r = std::bit_cast<std::uint64_t>(dright[lane]);
        for (std::size_t byte = 0; byte < 8; ++byte) {
            doubles.xmm[0][lane * 8U + byte] =
                static_cast<std::byte>((l >> (byte * 8U)) & 0xffU);
            doubles.xmm[1][lane * 8U + byte] =
                static_cast<std::byte>((r >> (byte * 8U)) & 0xffU);
        }
    }
    require(interpreter.step(doubles, memory) == StepResult::Executed,
            "ADDPD should execute");
    for (std::size_t lane = 0; lane < 2; ++lane) {
        std::uint64_t bits = 0;
        for (std::size_t byte = 0; byte < 8; ++byte)
            bits |= static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(doubles.xmm[0][lane * 8U + byte]))
                << (byte * 8U);
        require(std::bit_cast<double>(bits) == dleft[lane] + dright[lane],
                "ADDPD should add each packed double-precision lane");
    }
}


void test_sse_bitwise_and_movemask_family() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31300, 0x100};
    const std::array<std::byte, 4> andnps{
        std::byte{0x0f}, std::byte{0x55}, std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x31300, andnps), "ANDNPS instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31300;
    state.control_registers[4] = 1ULL << 9U;
    state.xmm[0].fill(std::byte{0xf0});
    state.xmm[1].fill(std::byte{0x0f});
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "ANDNPS should execute");
    for (const auto byte : state.xmm[0])
        require(byte == std::byte{0x0f},
                "ANDNPS should compute bitwise NOT(destination) AND source");

    const std::array<std::byte, 4> movmskps{
        std::byte{0x0f}, std::byte{0x50}, std::byte{0xd1}, std::byte{0xf4}
    };
    require(memory.write(0x31320, movmskps), "MOVMSKPS instruction should load");
    meridian::cpu::CpuState single_mask{};
    single_mask.rip = 0x31320;
    single_mask.control_registers[4] = 1ULL << 9U;
    single_mask.xmm[1][3] = std::byte{0x80};
    single_mask.xmm[1][11] = std::byte{0x80};
    require(interpreter.step(single_mask, memory) == StepResult::Executed,
            "MOVMSKPS should execute");
    require(single_mask.gpr[2] == 0x5,
            "MOVMSKPS should collect the sign bit of each 32-bit lane");

    const std::array<std::byte, 5> movmskpd{
        std::byte{0x66}, std::byte{0x0f}, std::byte{0x50},
        std::byte{0xc0}, std::byte{0xf4}
    };
    require(memory.write(0x31340, movmskpd), "MOVMSKPD instruction should load");
    meridian::cpu::CpuState double_mask{};
    double_mask.rip = 0x31340;
    double_mask.control_registers[4] = 1ULL << 9U;
    double_mask.xmm[0][7] = std::byte{0x80};
    require(interpreter.step(double_mask, memory) == StepResult::Executed,
            "MOVMSKPD should execute");
    require(double_mask.gpr[0] == 0x1,
            "MOVMSKPD should collect the sign bit of each 64-bit lane");
}


void test_ldmxcsr_stmxcsr() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31400, 0x200};
    const std::array<std::byte, 3> load_mxcsr{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x10}
    };
    const std::array<std::byte, 3> store_mxcsr{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x18}
    };
    require(memory.write(0x31400, load_mxcsr), "LDMXCSR instruction should load");
    require(memory.write(0x31410, store_mxcsr), "STMXCSR instruction should load");
    require(memory.write_le<std::uint32_t>(0x31480, 0x3f80U),
            "MXCSR source value should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31400;
    state.gpr[0] = 0x31480;
    state.control_registers[4] = 1ULL << 9U;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.mxcsr == 0x3f80U,
            "LDMXCSR should load a valid MXCSR value");
    state.rip = 0x31410;
    state.gpr[0] = 0x31484;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "STMXCSR should store the current MXCSR value");
    require(memory.read_le<std::uint32_t>(0x31484).value_or(0) == 0x3f80U,
            "STMXCSR should write the exact 32-bit control/status value");

    require(memory.write_le<std::uint32_t>(0x31480, 0xffffffffU),
            "invalid MXCSR value should load into test memory");
    state.rip = 0x31400;
    state.gpr[0] = 0x31480;
    const auto old_mxcsr = state.mxcsr;
    require(interpreter.step(state, memory) == StepResult::GeneralProtectionFault,
            "LDMXCSR should reject reserved bits outside MXCSR_MASK");
    require(state.mxcsr == old_mxcsr && state.rip == 0x31400,
            "invalid LDMXCSR should not partially update guest state");
}


void test_sse_compare_predicates() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31500, 0x100};
    const std::array<std::byte, 4> cmpps{
        std::byte{0x0f}, std::byte{0xc2}, std::byte{0xc1}, std::byte{0x00}
    };
    require(memory.write(0x31500, cmpps), "CMPPS instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31500;
    state.control_registers[4] = 1ULL << 9U;
    const std::array<float, 4> lhs{1.0F, 2.0F,
        std::bit_cast<float>(0x7fc00000U), 4.0F};
    const std::array<float, 4> rhs{1.0F, 3.0F,
        std::bit_cast<float>(0x7fc00000U), 4.0F};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const auto a = std::bit_cast<std::uint32_t>(lhs[lane]);
        const auto b = std::bit_cast<std::uint32_t>(rhs[lane]);
        for (std::size_t byte = 0; byte < 4; ++byte) {
            state.xmm[0][lane * 4U + byte] =
                static_cast<std::byte>((a >> (byte * 8U)) & 0xffU);
            state.xmm[1][lane * 4U + byte] =
                static_cast<std::byte>((b >> (byte * 8U)) & 0xffU);
        }
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CMPPS equality predicate should execute");
    const std::array<std::uint32_t, 4> expected{
        0xffffffffU, 0U, 0U, 0xffffffffU
    };
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t actual = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            actual |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(state.xmm[0][lane * 4U + byte]))
                << (byte * 8U);
        require(actual == expected[lane],
                "CMPPS EQ predicate should distinguish equal and unordered lanes");
    }

    const std::array<std::byte, 5> cmpss{
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0xc2},
        std::byte{0xc1}, std::byte{0x01}
    };
    require(memory.write(0x31520, cmpss), "CMPSS instruction should load");
    meridian::cpu::CpuState scalar{};
    scalar.rip = 0x31520;
    scalar.control_registers[4] = 1ULL << 9U;
    const float scalar_left = 2.0F;
    const float scalar_right = 3.0F;
    const auto left_bits = std::bit_cast<std::uint32_t>(scalar_left);
    const auto right_bits = std::bit_cast<std::uint32_t>(scalar_right);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        scalar.xmm[0][byte] = static_cast<std::byte>((left_bits >> (byte * 8U)) & 0xffU);
        scalar.xmm[1][byte] = static_cast<std::byte>((right_bits >> (byte * 8U)) & 0xffU);
    }
    scalar.xmm[0][4] = std::byte{0x55};
    require(interpreter.step(scalar, memory) == StepResult::Executed,
            "CMPSS less-than predicate should execute");
    require(scalar.xmm[0][0] == std::byte{0xff} &&
            scalar.xmm[0][1] == std::byte{0xff} &&
            scalar.xmm[0][2] == std::byte{0xff} &&
            scalar.xmm[0][3] == std::byte{0xff} &&
            scalar.xmm[0][4] == std::byte{0x55},
            "CMPSS should write an all-ones low lane and preserve upper bits");
}


void test_sse_sqrt_min_max_subset() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31600, 0x100};
    const std::array<std::byte, 4> sqrtps{
        std::byte{0x0f}, std::byte{0x51}, std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x31600, sqrtps), "SQRTPS instruction should load");
    meridian::cpu::CpuState sqrt_state{};
    sqrt_state.rip = 0x31600;
    sqrt_state.control_registers[4] = 1ULL << 9U;
    const std::array<float, 4> roots{4.0F, 9.0F, 16.0F, 25.0F};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const auto bits = std::bit_cast<std::uint32_t>(roots[lane]);
        for (std::size_t byte = 0; byte < 4; ++byte)
            sqrt_state.xmm[1][lane * 4U + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(sqrt_state, memory) == StepResult::Executed,
            "SQRTPS should execute");
    const std::array<float, 4> expected_roots{2.0F, 3.0F, 4.0F, 5.0F};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(sqrt_state.xmm[0][lane * 4U + byte]))
                << (byte * 8U);
        require(std::bit_cast<float>(bits) == expected_roots[lane],
                "SQRTPS should calculate each packed square root");
    }

    const std::array<std::byte, 5> minss{
        std::byte{0xf3}, std::byte{0x0f}, std::byte{0x5d},
        std::byte{0xc1}, std::byte{0xf4}
    };
    require(memory.write(0x31620, minss), "MINSS instruction should load");
    meridian::cpu::CpuState min_state{};
    min_state.rip = 0x31620;
    min_state.control_registers[4] = 1ULL << 9U;
    const auto five = std::bit_cast<std::uint32_t>(5.0F);
    const auto three = std::bit_cast<std::uint32_t>(3.0F);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        min_state.xmm[0][byte] = static_cast<std::byte>((five >> (byte * 8U)) & 0xffU);
        min_state.xmm[1][byte] = static_cast<std::byte>((three >> (byte * 8U)) & 0xffU);
    }
    min_state.xmm[0][4] = std::byte{0x77};
    require(interpreter.step(min_state, memory) == StepResult::Executed,
            "MINSS should execute");
    std::uint32_t min_bits = 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        min_bits |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(min_state.xmm[0][byte])) << (byte * 8U);
    require(std::bit_cast<float>(min_bits) == 3.0F &&
            min_state.xmm[0][4] == std::byte{0x77},
            "MINSS should select the lower scalar and preserve upper bits");
}


void test_xgetbv_xsetbv_xcr0() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31700, 0x100};
    const std::array<std::byte, 3> xgetbv{
        std::byte{0x0f}, std::byte{0x01}, std::byte{0xd0}
    };
    const std::array<std::byte, 3> xsetbv{
        std::byte{0x0f}, std::byte{0x01}, std::byte{0xd1}
    };
    require(memory.write(0x31700, xgetbv), "XGETBV instruction should load");
    require(memory.write(0x31710, xsetbv), "XSETBV instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31700;
    state.control_registers[4] = (1ULL << 18U) | (1ULL << 9U);
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.gpr[0] == 1 && state.gpr[2] == 0,
            "XGETBV should return the reset XCR0 value in EDX:EAX");

    state.rip = 0x31710;
    state.gpr[0] = 3;
    state.gpr[2] = 0;
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.xcr0 == 3,
            "XSETBV should enable the modeled x87 and SSE state components");
    state.rip = 0x31700;
    state.gpr[1] = 0;
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.gpr[0] == 3 && state.gpr[2] == 0,
            "XGETBV should return the updated XCR0 value");

    state.rip = 0x31710;
    state.gpr[0] = 7; // AVX state is not yet implemented.
    state.rip = 0x31710;
    state.gpr[0] = 7;
    require(interpreter.step(state, memory) == StepResult::Executed &&
            state.xcr0 == 7,
            "XSETBV should enable the modeled YMM upper-half state component");

    meridian::cpu::CpuState user{};
    user.rip = 0x31710;
    user.cpl = 3;
    user.control_registers[4] = (1ULL << 18U) | (1ULL << 9U);
    user.gpr[0] = 3;
    require(interpreter.step(user, memory) == StepResult::PrivilegeFault,
            "XSETBV should be privileged");

    meridian::cpu::CpuState disabled{};
    disabled.rip = 0x31700;
    require(interpreter.step(disabled, memory) == StepResult::UnsupportedInstruction,
            "XGETBV should require CR4.OSXSAVE");
}


void test_xsave_xrstor_ymm_state() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31800, 0x2000};
    const std::array<std::byte, 3> xsave{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x20}
    };
    const std::array<std::byte, 3> xrstor{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x28}
    };
    require(memory.write(0x31800, xsave), "XSAVE instruction should load");
    require(memory.write(0x31810, xrstor), "XRSTOR instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31800;
    state.gpr[0] = 0x31900;
    state.gpr[1] = 0;
    state.gpr[2] = 0;
    state.control_registers[4] = (1ULL << 18U) | (1ULL << 9U);
    state.xcr0 = 7;
    state.gpr[0] = 7;
    state.gpr[2] = 0;
    // RAX is also the memory base; load the state-area pointer after setting
    // the XSAVE request mask through the instruction's implicit EDX:EAX mask.
    // Use RDI as the base register in the instruction stream below instead.
    const std::array<std::byte, 3> xsave_rdi{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x27}
    };
    const std::array<std::byte, 3> xrstor_rdi{
        std::byte{0x0f}, std::byte{0xae}, std::byte{0x2f}
    };
    require(memory.write(0x31820, xsave_rdi), "XSAVE RDI form should load");
    require(memory.write(0x31830, xrstor_rdi), "XRSTOR RDI form should load");
    state.gpr[7] = 0x31900;
    state.gpr[0] = 7;
    state.gpr[2] = 0;
    state.mxcsr = 0x1f80;
    state.x87_control_word = 0x027f;
    state.xmm[0].fill(std::byte{0x11});
    state.ymm_high[0].fill(std::byte{0x22});
    state.st[0].fill(std::byte{0x33});
    const meridian::cpu::Interpreter interpreter{};
    state.rip = 0x31820;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "XSAVE should write the extended x87/SSE/YMM state image");
    require(memory.read_le<std::uint64_t>(0x31b00).value_or(0) == 7,
            "XSAVE should write XSTATE_BV for enabled state components");
    std::array<std::byte, 16> saved_ymm{};
    require(memory.read(0x31b40, saved_ymm) && saved_ymm == state.ymm_high[0],
            "XSAVE should store YMM upper halves after the legacy header");

    state.xmm[0].fill(std::byte{0});
    state.ymm_high[0].fill(std::byte{0});
    state.st[0].fill(std::byte{0});
    state.x87_control_word = 0x037f;
    state.rip = 0x31830;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "XRSTOR should restore the extended state image");
    require(state.x87_control_word == 0x027f &&
            state.xmm[0][0] == std::byte{0x11} &&
            state.ymm_high[0][0] == std::byte{0x22} &&
            state.st[0][0] == std::byte{0x33},
            "XRSTOR should restore x87, XMM, and YMM upper-half state");

    state.rip = 0x31820;
    state.gpr[7] = 0x31901;
    require(interpreter.step(state, memory) == StepResult::GeneralProtectionFault,
            "XSAVE should require 64-byte aligned memory");
}


void test_cpuid_xsave_leaf_and_osxsave() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x31a00, 64};
    const std::array<std::byte, 2> cpuid{
        std::byte{0x0f}, std::byte{0xa2}
    };
    require(memory.write(0x31a00, cpuid), "CPUID feature test should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x31a00;
    state.gpr[0] = 1;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPUID leaf 1 should execute");
    require((state.gpr[1] & (1U << 26U)) != 0 &&
            (state.gpr[1] & (1U << 27U)) == 0 &&
            (state.gpr[1] & (1U << 28U)) == 0,
            "CPUID should advertise XSAVE but not OSXSAVE or AVX before OS enablement");

    state.rip = 0x31a00;
    state.control_registers[4] = 1ULL << 18U;
    state.gpr[0] = 1;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPUID leaf 1 with OSXSAVE enabled should execute");
    require((state.gpr[1] & (1U << 27U)) != 0 &&
            (state.gpr[1] & (1U << 28U)) == 0,
            "CPUID should expose OSXSAVE only when CR4.OSXSAVE is enabled");

    state.rip = 0x31a00;
    state.gpr[0] = 0x0d;
    state.gpr[1] = 0;
    state.xcr0 = 7;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "CPUID leaf 0xD should execute");
    require(state.gpr[0] == 7 && state.gpr[3] == 832 &&
            state.gpr[1] == 832 && state.gpr[2] == 0,
            "CPUID leaf 0xD should report the supported and enabled xstate size");
}


void test_avx_vex_128_256_and_zero_upper() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32000, 0x200};
    const std::array<std::byte, 4> vaddps128{
        std::byte{0xc5}, std::byte{0xf0}, std::byte{0x58}, std::byte{0xc2}
    };
    const std::array<std::byte, 4> vaddps256{
        std::byte{0xc5}, std::byte{0xf4}, std::byte{0x58}, std::byte{0xc2}
    };
    const std::array<std::byte, 3> vzeroupper{
        std::byte{0xc5}, std::byte{0x80}, std::byte{0x77}
    };
    const std::array<std::byte, 3> vzeroall{
        std::byte{0xc5}, std::byte{0x84}, std::byte{0x77}
    };
    require(memory.write(0x32000, vaddps128), "VADDPS.128 instruction should load");
    require(memory.write(0x32010, vaddps256), "VADDPS.256 instruction should load");
    require(memory.write(0x32020, vzeroupper), "VZEROUPPER instruction should load");
    require(memory.write(0x32030, vzeroall), "VZEROALL instruction should load");

    meridian::cpu::CpuState state{};
    state.rip = 0x32000;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const std::array<float, 8> left{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<float, 8> right{10, 20, 30, 40, 50, 60, 70, 80};
    for (std::size_t lane = 0; lane < 8; ++lane) {
        const auto a = std::bit_cast<std::uint32_t>(left[lane]);
        const auto b = std::bit_cast<std::uint32_t>(right[lane]);
        auto& dest = lane < 4 ? state.xmm[1] : state.ymm_high[1];
        auto& src = lane < 4 ? state.xmm[2] : state.ymm_high[2];
        const std::size_t offset = (lane % 4U) * 4U;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            dest[offset + byte] = static_cast<std::byte>((a >> (byte * 8U)) & 0xffU);
            src[offset + byte] = static_cast<std::byte>((b >> (byte * 8U)) & 0xffU);
        }
    }
    state.ymm_high[0].fill(std::byte{0xaa});
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VADDPS.128 should execute");
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(state.xmm[0][lane * 4U + byte]))
                << (byte * 8U);
        require(std::bit_cast<float>(bits) == left[lane] + right[lane],
                "VADDPS.128 should compute four packed lanes");
    }
    for (const auto byte : state.ymm_high[0])
        require(byte == std::byte{0},
                "VEX.128 destination writes should zero the upper YMM half");

    state.rip = 0x32010;
    state.ymm_high[0].fill(std::byte{0xaa});
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VADDPS.256 should execute");
    for (std::size_t lane = 4; lane < 8; ++lane) {
        std::uint32_t bits = 0;
        const std::size_t offset = (lane - 4U) * 4U;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(state.ymm_high[0][offset + byte]))
                << (byte * 8U);
        require(std::bit_cast<float>(bits) == left[lane] + right[lane],
                "VADDPS.256 should compute upper packed lanes");
    }

    state.rip = 0x32020;
    for (auto& reg : state.ymm_high) reg.fill(std::byte{0xaa});
    const auto low_before = state.xmm;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VZEROUPPER should execute");
    require(state.xmm == low_before,
            "VZEROUPPER should preserve all low XMM halves");
    for (const auto& reg : state.ymm_high)
        for (const auto byte : reg)
            require(byte == std::byte{0},
                    "VZEROUPPER should clear every YMM upper half");

    state.rip = 0x32030;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VZEROALL should execute");
    for (const auto& reg : state.xmm)
        for (const auto byte : reg)
            require(byte == std::byte{0},
                    "VZEROALL should clear all XMM low halves");
    for (const auto& reg : state.ymm_high)
        for (const auto byte : reg)
            require(byte == std::byte{0},
                    "VZEROALL should clear all YMM upper halves");
}


void test_avx_double_and_bitwise_operations() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32100, 0x100};
    const std::array<std::byte, 4> vaddpd{
        std::byte{0xc5}, std::byte{0xf1}, std::byte{0x58}, std::byte{0xc2}
    };
    const std::array<std::byte, 4> vandnps{
        std::byte{0xc5}, std::byte{0xf0}, std::byte{0x55}, std::byte{0xc2}
    };
    require(memory.write(0x32100, vaddpd), "VADDPD instruction should load");
    require(memory.write(0x32110, vandnps), "VANDNPS instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32100;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const std::array<double, 2> left{1.5, 2.5};
    const std::array<double, 2> right{2.5, 3.5};
    for (std::size_t lane = 0; lane < 2; ++lane) {
        const auto a = std::bit_cast<std::uint64_t>(left[lane]);
        const auto b = std::bit_cast<std::uint64_t>(right[lane]);
        for (std::size_t byte = 0; byte < 8; ++byte) {
            state.xmm[1][lane * 8U + byte] =
                static_cast<std::byte>((a >> (byte * 8U)) & 0xffU);
            state.xmm[2][lane * 8U + byte] =
                static_cast<std::byte>((b >> (byte * 8U)) & 0xffU);
        }
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VADDPD.128 should execute");
    for (std::size_t lane = 0; lane < 2; ++lane) {
        std::uint64_t bits = 0;
        for (std::size_t byte = 0; byte < 8; ++byte)
            bits |= static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(state.xmm[0][lane * 8U + byte]))
                << (byte * 8U);
        require(std::bit_cast<double>(bits) == left[lane] + right[lane],
                "VADDPD should add packed double-precision lanes");
    }

    state.rip = 0x32110;
    state.xmm[1].fill(std::byte{0xf0});
    state.xmm[2].fill(std::byte{0x0f});
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VANDNPS should execute");
    for (const auto byte : state.xmm[0])
        require(byte == std::byte{0x0f},
                "VANDNPS should compute NOT(source1) AND source2");
}


void test_avx2_packed_integer_operations_subset() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32200, 0x100};
    const std::array<std::byte, 5> vpaddq{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xd4}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpxor{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xef}, std::byte{0xc2}
    };
    require(memory.write(0x32200, vpaddq), "VPADDQ instruction should load");
    require(memory.write(0x32210, vpxor), "VPXOR instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32200;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const auto put = [&](std::array<std::byte, 16>& target,
                         std::size_t lane, std::uint64_t value) {
        for (std::size_t byte = 0; byte < 8; ++byte)
            target[lane * 8U + byte] =
                static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    };
    const auto get = [&](const std::array<std::byte, 16>& source,
                         std::size_t lane) {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < 8; ++byte)
            value |= static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(source[lane * 8U + byte]))
                << (byte * 8U);
        return value;
    };
    put(state.xmm[1], 0, 10);
    put(state.xmm[1], 1, 20);
    put(state.ymm_high[1], 0, 30);
    put(state.ymm_high[1], 1, 40);
    put(state.xmm[2], 0, 3);
    put(state.xmm[2], 1, 4);
    put(state.ymm_high[2], 0, 5);
    put(state.ymm_high[2], 1, 6);
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPADDQ.256 should execute");
    require(get(state.xmm[0], 0) == 13 && get(state.xmm[0], 1) == 24 &&
            get(state.ymm_high[0], 0) == 35 &&
            get(state.ymm_high[0], 1) == 46,
            "VPADDQ should add four independent 64-bit YMM lanes");

    state.rip = 0x32210;
    state.xmm[1].fill(std::byte{0xf0});
    state.ymm_high[1].fill(std::byte{0xf0});
    state.xmm[2].fill(std::byte{0x0f});
    state.ymm_high[2].fill(std::byte{0x0f});
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPXOR.256 should execute");
    for (const auto byte : state.xmm[0])
        require(byte == std::byte{0xff},
                "VPXOR should XOR the lower 128-bit lane");
    for (const auto byte : state.ymm_high[0])
        require(byte == std::byte{0xff},
                "VPXOR should XOR the upper 128-bit lane");
}


void test_avx2_vmovdqa_vmovdqu_memory_forms() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32300, 0x200};
    const std::array<std::byte, 5> vmovdqu_load{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x06},
        std::byte{0x6f}, std::byte{0x00}
    };
    const std::array<std::byte, 6> vmovdqu_store{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x06},
        std::byte{0x7f}, std::byte{0x40}, std::byte{0x20}
    };
    const std::array<std::byte, 5> vmovdqa_load{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x05},
        std::byte{0x6f}, std::byte{0x00}
    };
    require(memory.write(0x32300, vmovdqu_load), "VMOVDQU load should load");
    require(memory.write(0x32310, vmovdqu_store), "VMOVDQU store should load");
    require(memory.write(0x32320, vmovdqa_load), "VMOVDQA load should load");
    std::array<std::byte, 32> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<std::byte>(i + 1U);
    require(memory.write(0x32380, input), "YMM memory source should load");

    meridian::cpu::CpuState state{};
    state.rip = 0x32300;
    state.gpr[0] = 0x32380;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VMOVDQU should load an unaligned-capable 256-bit operand");
    require(std::equal(state.xmm[0].begin(), state.xmm[0].end(), input.begin()) &&
            std::equal(state.ymm_high[0].begin(), state.ymm_high[0].end(),
                       input.begin() + 16),
            "VMOVDQU should split the 256-bit value across XMM and YMM-high state");

    state.rip = 0x32310;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VMOVDQU should store a 256-bit register to memory");
    std::array<std::byte, 32> output{};
    require(memory.read(0x323a0, output) && output == input,
            "VMOVDQU store should write all 256 bits");

    state.rip = 0x32320;
    state.gpr[0] = 0x32381;
    require(interpreter.step(state, memory) == StepResult::GeneralProtectionFault,
            "VMOVDQA should reject non-32-byte-aligned YMM memory operands");
}


void test_avx2_vpshufd_vshufps() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32400, 0x100};
    const std::array<std::byte, 6> vpshufd{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x05},
        std::byte{0x70}, std::byte{0xc1}, std::byte{0x1b}
    };
    const std::array<std::byte, 6> vshufps{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x74},
        std::byte{0xc6}, std::byte{0xc2}, std::byte{0x4e}
    };
    require(memory.write(0x32400, vpshufd), "VPSHUFD instruction should load");
    require(memory.write(0x32410, vshufps), "VSHUFPS instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32400;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const auto set_dword = [](std::array<std::byte, 16>& target,
                              std::size_t lane, std::uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte)
            target[lane * 4U + byte] =
                static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    };
    const auto get_dword = [](const std::array<std::byte, 16>& source,
                              std::size_t lane) {
        std::uint32_t value = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            value |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(source[lane * 4U + byte]))
                << (byte * 8U);
        return value;
    };
    for (std::size_t lane = 0; lane < 4; ++lane) {
        set_dword(state.xmm[1], lane, static_cast<std::uint32_t>(lane + 1U));
        set_dword(state.ymm_high[1], lane, static_cast<std::uint32_t>(lane + 5U));
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPSHUFD.256 should execute");
    const std::array<std::uint32_t, 4> low_expected{4, 3, 2, 1};
    const std::array<std::uint32_t, 4> high_expected{8, 7, 6, 5};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        require(get_dword(state.xmm[0], lane) == low_expected[lane],
                "VPSHUFD should shuffle the lower 128-bit lane");
        require(get_dword(state.ymm_high[0], lane) == high_expected[lane],
                "VPSHUFD should shuffle the upper 128-bit lane independently");
    }

    state.rip = 0x32410;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        set_dword(state.xmm[1], lane, static_cast<std::uint32_t>(10U + lane));
        set_dword(state.xmm[2], lane, static_cast<std::uint32_t>(20U + lane));
        set_dword(state.ymm_high[1], lane, static_cast<std::uint32_t>(30U + lane));
        set_dword(state.ymm_high[2], lane, static_cast<std::uint32_t>(40U + lane));
    }
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VSHUFPS.256 should execute");
    require(get_dword(state.xmm[0], 0) == 12 &&
            get_dword(state.xmm[0], 1) == 13 &&
            get_dword(state.xmm[0], 2) == 20 &&
            get_dword(state.xmm[0], 3) == 21,
            "VSHUFPS should select lanes from both lower source halves");
    require(get_dword(state.ymm_high[0], 0) == 32 &&
            get_dword(state.ymm_high[0], 1) == 33 &&
            get_dword(state.ymm_high[0], 2) == 40 &&
            get_dword(state.ymm_high[0], 3) == 41,
            "VSHUFPS should independently shuffle both upper source halves");
}


void test_avx2_vpshufb_128bit_lane_behavior() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32500, 0x100};
    const std::array<std::byte, 5> vpshufb{
        std::byte{0xc4}, std::byte{0xe2}, std::byte{0x75},
        std::byte{0x00}, std::byte{0xc2}
    };
    require(memory.write(0x32500, vpshufb), "VPSHUFB instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32500;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    for (std::size_t i = 0; i < 16; ++i) {
        state.xmm[1][i] = static_cast<std::byte>(i);
        state.xmm[2][i] = static_cast<std::byte>(15U - i);
        state.ymm_high[1][i] = static_cast<std::byte>(16U + i);
        state.ymm_high[2][i] = static_cast<std::byte>(15U - i);
    }
    state.xmm[2][0] = std::byte{0x80}; // High bit forces a zero result.
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPSHUFB.256 should execute");
    require(state.xmm[0][0] == std::byte{0},
            "VPSHUFB should zero a byte when control bit 7 is set");
    for (std::size_t i = 1; i < 16; ++i)
        require(state.xmm[0][i] == static_cast<std::byte>(15U - i),
                "VPSHUFB should select bytes within the lower 128-bit lane");
    for (std::size_t i = 0; i < 16; ++i)
        require(state.ymm_high[0][i] == static_cast<std::byte>(31U - i),
                "VPSHUFB should shuffle the upper 128-bit lane independently");
}


void test_avx_vperm2f128_vperm2i128() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32600, 0x100};
    const std::array<std::byte, 6> vperm2f128{
        std::byte{0xc4}, std::byte{0xe3}, std::byte{0x74},
        std::byte{0x06}, std::byte{0xc2}, std::byte{0x21}
    };
    const std::array<std::byte, 6> vperm2i128{
        std::byte{0xc4}, std::byte{0xe3}, std::byte{0x75},
        std::byte{0x46}, std::byte{0xc2}, std::byte{0x21}
    };
    require(memory.write(0x32600, vperm2f128), "VPERM2F128 instruction should load");
    require(memory.write(0x32610, vperm2i128), "VPERM2I128 instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32600;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    for (std::size_t i = 0; i < 16; ++i) {
        state.xmm[1][i] = static_cast<std::byte>(i + 1U);
        state.ymm_high[1][i] = static_cast<std::byte>(i + 17U);
        state.xmm[2][i] = static_cast<std::byte>(i + 33U);
        state.ymm_high[2][i] = static_cast<std::byte>(i + 49U);
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPERM2F128 should execute");
    require(state.xmm[0] == state.ymm_high[1] &&
            state.ymm_high[0] == state.xmm[2],
            "VPERM2F128 imm8 0x21 should select source1.high and source2.low");

    state.rip = 0x32610;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPERM2I128 should execute");
    require(state.xmm[0] == state.ymm_high[1] &&
            state.ymm_high[0] == state.xmm[2],
            "VPERM2I128 should select the same 128-bit source halves");
}


void test_avx2_packed_compare_and_vpmovmskb() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32700, 0x100};
    const std::array<std::byte, 5> vpcmpeqd{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0x76}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpmovmskb{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x05},
        std::byte{0xd7}, std::byte{0xd8}
    };
    require(memory.write(0x32700, vpcmpeqd), "VPCMPEQD instruction should load");
    require(memory.write(0x32710, vpmovmskb), "VPMOVMSKB instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32700;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    const std::array<std::uint32_t, 8> left{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::uint32_t, 8> right{1, 0, 3, 0, 5, 0, 7, 0};
    for (std::size_t lane = 0; lane < 8; ++lane) {
        auto& a = lane < 4 ? state.xmm[1] : state.ymm_high[1];
        auto& b = lane < 4 ? state.xmm[2] : state.ymm_high[2];
        const std::size_t offset = (lane % 4U) * 4U;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            a[offset + byte] = static_cast<std::byte>((left[lane] >> (byte * 8U)) & 0xffU);
            b[offset + byte] = static_cast<std::byte>((right[lane] >> (byte * 8U)) & 0xffU);
        }
    }
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPCMPEQD.256 should execute");
    for (std::size_t lane = 0; lane < 8; ++lane) {
        const auto& value = lane < 4 ? state.xmm[0] : state.ymm_high[0];
        const std::size_t offset = (lane % 4U) * 4U;
        const std::byte expected = (lane % 2U) == 0
            ? std::byte{0xff} : std::byte{0x00};
        require(value[offset] == expected,
                "VPCMPEQD should create all-ones masks for equal dword lanes");
    }
    state.rip = 0x32710;
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPMOVMSKB should execute");
    require(state.gpr[3] == 0x0f0f0f0fU,
            "VPMOVMSKB should collect 32 byte sign bits from a YMM register");
}


void test_avx2_packed_byte_word_arithmetic() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32800, 0x100};
    const std::array<std::byte, 5> vpaddb{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xfc}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpsubw{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xf9}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpmullw{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xd5}, std::byte{0xc2}
    };
    require(memory.write(0x32800, vpaddb), "VPADDB instruction should load");
    require(memory.write(0x32810, vpsubw), "VPSUBW instruction should load");
    require(memory.write(0x32820, vpmullw), "VPMULLW instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32800;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    state.xmm[1].fill(std::byte{0xfe});
    state.xmm[2].fill(std::byte{0x05});
    state.ymm_high[1].fill(std::byte{0xfe});
    state.ymm_high[2].fill(std::byte{0x05});
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPADDB.256 should execute");
    for (const auto byte : state.xmm[0])
        require(byte == std::byte{0x03},
                "VPADDB should wrap independently in every byte lane");
    for (const auto byte : state.ymm_high[0])
        require(byte == std::byte{0x03},
                "VPADDB should wrap upper YMM byte lanes");

    state.rip = 0x32810;
    state.xmm[1].fill(std::byte{0x00});
    state.xmm[2].fill(std::byte{0x00});
    state.ymm_high[1].fill(std::byte{0x00});
    state.ymm_high[2].fill(std::byte{0x00});
    // Set each 16-bit lane to 2 and 5, respectively.
    for (std::size_t lane = 0; lane < 8; ++lane) {
        auto& a = lane < 8 ? state.xmm[1] : state.ymm_high[1];
        auto& b = lane < 8 ? state.xmm[2] : state.ymm_high[2];
        const std::size_t offset = (lane % 8U) * 2U;
        a[offset] = std::byte{0x02};
        b[offset] = std::byte{0x05};
    }
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPSUBW.256 should execute");
    require(state.xmm[0][0] == std::byte{0xfd} &&
            state.xmm[0][1] == std::byte{0xff},
            "VPSUBW should wrap signed-looking 16-bit lanes modulo 2^16");

    state.rip = 0x32820;
    for (auto& reg : state.xmm) reg.fill(std::byte{0});
    for (auto& reg : state.ymm_high) reg.fill(std::byte{0});
    state.xmm[1][0] = std::byte{0x03};
    state.xmm[1][1] = std::byte{0x00};
    state.xmm[2][0] = std::byte{0x04};
    state.xmm[2][1] = std::byte{0x00};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPMULLW.256 should execute");
    require(state.xmm[0][0] == std::byte{0x0c} &&
            state.xmm[0][1] == std::byte{0x00},
            "VPMULLW should keep the low 16 bits of each word product");
}


void test_avx2_saturating_and_minmax_operations() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x32900, 0x100};
    const std::array<std::byte, 5> vpaddsb{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xec}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpaddusb{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xdc}, std::byte{0xc2}
    };
    const std::array<std::byte, 5> vpsubsw{
        std::byte{0xc4}, std::byte{0xe1}, std::byte{0x75},
        std::byte{0xe9}, std::byte{0xc2}
    };
    require(memory.write(0x32900, vpaddsb), "VPADDSB instruction should load");
    require(memory.write(0x32910, vpaddusb), "VPADDUSB instruction should load");
    require(memory.write(0x32920, vpsubsw), "VPSUBSW instruction should load");
    meridian::cpu::CpuState state{};
    state.rip = 0x32900;
    state.control_registers[4] = (1ULL << 9U) | (1ULL << 18U);
    state.xcr0 = 7;
    state.xmm[1].fill(std::byte{0x7f});
    state.xmm[2].fill(std::byte{0x01});
    state.ymm_high[1].fill(std::byte{0x7f});
    state.ymm_high[2].fill(std::byte{0x01});
    const meridian::cpu::Interpreter interpreter{};
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPADDSB should execute");
    require(state.xmm[0][0] == std::byte{0x7f} &&
            state.ymm_high[0][0] == std::byte{0x7f},
            "VPADDSB should saturate positive signed-byte overflow");

    state.rip = 0x32910;
    state.xmm[1].fill(std::byte{0xff});
    state.xmm[2].fill(std::byte{0x02});
    state.ymm_high[1].fill(std::byte{0xff});
    state.ymm_high[2].fill(std::byte{0x02});
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPADDUSB should execute");
    require(state.xmm[0][0] == std::byte{0xff} &&
            state.ymm_high[0][0] == std::byte{0xff},
            "VPADDUSB should saturate unsigned-byte overflow at 255");

    state.rip = 0x32920;
    state.xmm[1].fill(std::byte{0x00});
    state.xmm[2].fill(std::byte{0x00});
    state.ymm_high[1].fill(std::byte{0x00});
    state.ymm_high[2].fill(std::byte{0x00});
    for (std::size_t i = 0; i < 16; i += 2) {
        state.xmm[1][i] = state.ymm_high[1][i] = std::byte{0x00};
        state.xmm[1][i + 1] = state.ymm_high[1][i + 1] = std::byte{0x80};
        state.xmm[2][i] = state.ymm_high[2][i] = std::byte{0x01};
    }
    require(interpreter.step(state, memory) == StepResult::Executed,
            "VPSUBSW should execute");
    require(state.xmm[0][0] == std::byte{0x00} &&
            state.xmm[0][1] == std::byte{0x80},
            "VPSUBSW should saturate negative signed-word overflow");
}


void test_indirect_call_jump_and_push_group() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x33000, 0x1000};
    const std::array<std::byte, 2> call_rax{
        std::byte{0xff}, std::byte{0xd0}
    };
    const std::array<std::byte, 2> call_memory{
        std::byte{0xff}, std::byte{0x10}
    };
    const std::array<std::byte, 2> jump_rax{
        std::byte{0xff}, std::byte{0xe0}
    };
    const std::array<std::byte, 2> push_rax{
        std::byte{0xff}, std::byte{0xf0}
    };
    const std::array<std::byte, 1> halt{std::byte{0xf4}};
    require(memory.write(0x33000, call_rax), "indirect CALL RAX should load");
    require(memory.write(0x33010, halt), "indirect CALL target should load");
    require(memory.write(0x33020, call_memory), "indirect CALL memory should load");
    require(memory.write(0x33030, halt), "indirect memory CALL target should load");
    require(memory.write(0x33040, jump_rax), "indirect JMP RAX should load");
    require(memory.write(0x33050, halt), "indirect JMP target should load");
    require(memory.write(0x33060, push_rax), "PUSH RAX group form should load");
    require(memory.write_le<std::uint64_t>(0x33080, 0x33030),
            "indirect CALL pointer should load");

    const meridian::cpu::Interpreter interpreter{};
    meridian::cpu::CpuState call_state{};
    call_state.rip = 0x33000;
    call_state.gpr[0] = 0x33010;
    call_state.gpr[4] = 0x33ff0;
    require(interpreter.step(call_state, memory) == StepResult::Executed,
            "CALL RAX should execute");
    require(call_state.rip == 0x33010,
            "CALL RAX should branch to the target in RAX");
    require(call_state.gpr[4] == 0x33fe8,
            "CALL RAX should decrement RSP by eight bytes");
    require(memory.read_le<std::uint64_t>(0x33fe8).value_or(0) == 0x33002,
            "CALL RAX should store the correct return address");
    require(interpreter.step(call_state, memory) == StepResult::Halted,
            "indirect CALL target should execute");

    meridian::cpu::CpuState memory_call{};
    memory_call.rip = 0x33020;
    memory_call.gpr[0] = 0x33080;
    memory_call.gpr[4] = 0x33ff0;
    require(interpreter.step(memory_call, memory) == StepResult::Executed &&
            memory_call.rip == 0x33030,
            "CALL [RAX] should load its target from guest memory");
    require(memory.read_le<std::uint64_t>(0x33fe8).value_or(0) == 0x33022,
            "CALL [RAX] should push the decoded next RIP");

    meridian::cpu::CpuState jump_state{};
    jump_state.rip = 0x33040;
    jump_state.gpr[0] = 0x33050;
    require(interpreter.step(jump_state, memory) == StepResult::Executed &&
            jump_state.rip == 0x33050,
            "JMP RAX should transfer control without pushing");

    meridian::cpu::CpuState push_state{};
    push_state.rip = 0x33060;
    push_state.gpr[0] = 0x123456789abcdef0ULL;
    push_state.gpr[4] = 0x33ff0;
    require(interpreter.step(push_state, memory) == StepResult::Executed &&
            push_state.rip == 0x33062 &&
            push_state.gpr[4] == 0x33fe8,
            "PUSH r/m64 should push the selected operand");
    require(memory.read_le<std::uint64_t>(0x33fe8).value_or(0) ==
                0x123456789abcdef0ULL,
            "PUSH RAX should preserve the full 64-bit value");
}


void test_lea_and_movsxd_addressing() {
    using meridian::cpu::StepResult;
    meridian::core::GuestMemory memory{0x33100, 0x200};
    const std::array<std::byte, 6> lea_sib{
        std::byte{0x48}, std::byte{0x8d}, std::byte{0x44},
        std::byte{0x8b}, std::byte{0x10}, std::byte{0xf4}
    };
    const std::array<std::byte, 4> movsxd_register{
        std::byte{0x48}, std::byte{0x63}, std::byte{0xd1}, std::byte{0xf4}
    };
    const std::array<std::byte, 5> movsxd_memory{
        std::byte{0x4c}, std::byte{0x63}, std::byte{0x43},
        std::byte{0x04}, std::byte{0xf4}
    };
    require(memory.write(0x33100, lea_sib), "LEA SIB instruction should load");
    require(memory.write(0x33120, movsxd_register),
            "MOVSXD register instruction should load");
    require(memory.write(0x33140, movsxd_memory),
            "MOVSXD memory instruction should load");
    require(memory.write_le<std::uint32_t>(0x33184, 0xfffffffeU),
            "MOVSXD memory source should load");
    const meridian::cpu::Interpreter interpreter{};

    meridian::cpu::CpuState lea_state{};
    lea_state.rip = 0x33100;
    lea_state.gpr[3] = 0x1000; // RBX base.
    lea_state.gpr[1] = 3;      // RCX index.
    require(interpreter.step(lea_state, memory) == StepResult::Executed,
            "LEA with SIB addressing should execute");
    require(lea_state.gpr[0] == 0x101c,
            "LEA should compute base + scaled index + displacement");
    require(interpreter.step(lea_state, memory) == StepResult::Halted,
            "LEA program should reach HLT");

    meridian::cpu::CpuState register_state{};
    register_state.rip = 0x33120;
    register_state.gpr[1] = 0x80000001ULL;
    require(interpreter.step(register_state, memory) == StepResult::Executed,
            "MOVSXD register form should execute");
    require(register_state.gpr[2] == 0xffffffff80000001ULL,
            "MOVSXD should sign-extend the source dword into a qword");

    meridian::cpu::CpuState memory_state{};
    memory_state.rip = 0x33140;
    memory_state.gpr[3] = 0x33180;
    require(interpreter.step(memory_state, memory) == StepResult::Executed,
            "MOVSXD memory form should execute");
    require(memory_state.gpr[8] == 0xfffffffffffffffeULL,
            "MOVSXD should sign-extend a memory dword into an extended GPR");
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
    require(state.control_registers[3] == 0 && state.efer == 0 && state.cpl == 0,
            "control registers, EFER, and CPL should reset");
    require(state.star == 0 && state.lstar == 0 && state.fs_base == 0 && state.tsc == 0,
            "modeled MSR state should reset");
    require(state.cs == 0 && state.ss == 0 && state.gs == 0,
            "segment selectors should reset");
    require(state.xcr0 == 1,
            "XCR0 should reset with only the x87 state component enabled");
    require(state.ymm_high[0][0] == std::byte{0},
            "YMM upper-half state should reset to zero");
    require(state.xmm[3][2] == std::byte{0}, "vector registers should reset");
}
} // namespace

int main() {
    try {
        test_guest_memory();
        test_virtual_address_space();
        test_virtual_address_protection_changes();
        test_virtual_address_typed_access();
        test_interpreter_virtual_address_space();
        test_lea_effective_address();
        test_movsxd_registers();
        test_movzx_registers();
        test_movsx_registers();
        test_mov_immediate_group_c7();
        test_mov_immediate_c7_memory();
        test_mov_immediate_c7_rip_relative();
        test_c7_memory_write_permissions_in_virtual_space();
        test_c7_cross_page_permission_fault_is_atomic();
        test_movsx_movzx_memory_operands();
        test_movsxd_memory_operands();
        test_movzx_virtual_read_permissions();
        test_immediate_and_or_registers();
        test_register_and_memory_or_and();
        test_sign_extended_imm8_group1();
        test_all_short_condition_codes();
        test_near_unsigned_condition_branch();
        test_setcc_register_forms();
        test_cmovcc_register_forms();
        test_setcc_memory_operands();
        test_group1_immediate_memory_destinations();
        test_x64_page_table_walker();
        test_interpreter_through_x64_page_tables();
        test_control_register_moves_and_invlpg();
        test_rdmsr_wrmsr_and_privilege();
        test_syscall_sysret_and_swapgs();
        test_cpuid_rdtsc_and_rdtscp();
        test_pe32_plus_image_loader();
        test_xvd_xvc_header_metadata_reader();
        test_xvc_descriptor_region_metadata();
        test_bounded_execution_engine();
        test_sse_sse2_xmm_instructions();
        test_sse2_packed_integer_arithmetic();
        test_sse2_packed_compare_and_movemask();
        test_sse2_pshufd_and_shufps();
        test_unsupported_legacy_prefix_combinations();
        test_fxsave_fxrstor_state_round_trip();
        test_movd_movq_xmm_transfers();
        test_sse_packed_and_scalar_moves();
        test_sse_floating_arithmetic_subset();
        test_sse_bitwise_and_movemask_family();
        test_ldmxcsr_stmxcsr();
        test_sse_compare_predicates();
        test_sse_sqrt_min_max_subset();
        test_xgetbv_xsetbv_xcr0();
        test_xsave_xrstor_ymm_state();
        test_cpuid_xsave_leaf_and_osxsave();
        test_avx_vex_128_256_and_zero_upper();
        test_avx_double_and_bitwise_operations();
        test_avx2_packed_integer_operations_subset();
        test_avx2_vmovdqa_vmovdqu_memory_forms();
        test_avx2_vpshufd_vshufps();
        test_avx2_vpshufb_128bit_lane_behavior();
        test_avx_vperm2f128_vperm2i128();
        test_avx2_packed_compare_and_vpmovmskb();
        test_avx2_packed_byte_word_arithmetic();
        test_avx2_saturating_and_minmax_operations();
        test_indirect_call_jump_and_push_group();
        test_lea_and_movsxd_addressing();

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
