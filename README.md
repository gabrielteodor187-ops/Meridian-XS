# Meridian-XS

An experimental, independently implemented research project exploring Xbox Series X|S emulation on PC.

> **Current status:** early CPU/memory foundations. This project does not boot Xbox software or run commercial games.

## Implemented

- C++23 / CMake project.
- Bounded guest-memory region with checked reads and writes.
- Little-endian typed memory access.
- x86-64 architectural CPU state container.
- Initial instruction interpreter: NOP, HLT, and MOV register-immediate.
- Register-only 64-bit ADD, SUB, XOR, and CMP instructions.
- Short JMP, JZ, and JNZ branches plus near 32-bit JZ/JNZ branches.
- Basic arithmetic status flags: carry, parity, auxiliary carry, zero, sign, and overflow.
- 64-bit PUSH/POP for general-purpose registers, including REX.B register extension.
- Near relative CALL and RET with checked guest-stack access.
- 64-bit MOV register loads/stores using register-indirect base plus signed 8-bit or 32-bit displacement.
- REX.R/REX.B extension for memory MOV register and base selection.
- CTest-based tests for memory, CPU state, and supported instructions.
- GitHub Actions build-and-test workflow.

## Build and test

Requires CMake 3.24+ and a C++23 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Not implemented

This is not a usable Xbox emulator yet. Instruction coverage is tiny; there is no complete x86-64 decoder, SIB or RIP-relative memory addressing, virtual memory, privilege model, interrupt handling, Xbox kernel/system-service layer, GPU emulation, boot flow, or game compatibility. The CPU interpreter is an early educational foundation, not a commercial-game execution engine.

## Development direction

1. Expand x86-64 decoding, including memory operands and immediate arithmetic.
2. Build a mapped address-space model with page permissions.
3. Add interrupts, scheduling, and system-call/platform abstractions.
4. Research graphics command translation and console-specific interfaces using public documentation.

All implementation is intended to be original code based on public information. No proprietary SDKs, firmware, keys, or game data are included.
