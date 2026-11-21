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
- Register-only 64-bit ADD, SUB, XOR, and CMP using sign-extended imm32 operands.
- SIB memory addressing with base, scaled index, optional no-base disp32, and REX.X/REX.B extensions.
- RIP-relative 64-bit MOV loads and stores using signed disp32 offsets, including correct REX.B handling.
- 32-bit register and immediate ADD, SUB, XOR, and CMP with 32-bit flag behaviour and architectural zero-extension into 64-bit GPRs.
- 32-bit register-to-register MOV (both operand directions), including REX.R/REX.B extended registers and zero-extension into 64-bit GPRs.
- 32-bit MOV loads and stores using supported base/displacement and extended base-register addressing, with zero-extension on loads.
- Register-only 32-bit and 64-bit TEST instructions with logical flag updates and no operand modification.
- 32-bit and 64-bit memory-destination ADD, SUB, XOR, and CMP using base, disp8, disp32, SIB-scaled index, no-base SIB, and RIP-relative addressing.
- Page-granular virtual address space with explicit read/write/execute permissions, cross-page access, mapping, unmapping, atomic range protection changes, and typed little-endian read/write/instruction-fetch helpers.
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

This is not a usable Xbox emulator yet. Instruction coverage is tiny; there is no complete x86-64 decoder, integrated CPU page-table walking or privilege model, interrupt handling, Xbox kernel/system-service layer, GPU emulation, boot flow, or game compatibility. The CPU interpreter is an early educational foundation, not a commercial-game execution engine.

## Development direction

1. Integrate the page-granular virtual address-space primitive with CPU memory accesses and page-table semantics.
2. Expand x86-64 decoding, including broader memory operands, arithmetic, and control flow.
3. Add interrupts, scheduling, and system-call/platform abstractions.
4. Research graphics command translation and console-specific interfaces using public documentation.

All implementation is intended to be original code based on public information. No proprietary SDKs, firmware, keys, or game data are included.


### v0.20.0 — CPU virtual-address-space execution

The interpreter can now execute against either the original bounded guest-memory region or the page-based virtual address space. Instruction bytes and immediates use execute-checked fetches; data loads, stores, and stack operations use read/write-checked accesses. Automated tests cover execution from read/execute pages, data access, denied instruction fetches, and denied writes. This remains a small x86-64 instruction subset and is not full Xbox virtual-memory or hardware emulation.


### v0.21.0 — LEA effective-address calculation

Added 32-bit and 64-bit LEA for the supported ModRM/SIB addressing forms, including base-plus-index scaling, signed displacements, RIP-relative addressing, and REX.R/X/B extended registers. LEA computes an address without reading guest memory. Automated tests cover SIB, RIP-relative, and extended-register cases. The instruction decoder remains intentionally incomplete.


### v0.22.0 — MOVSXD register form

Added the REX.W register-to-register form of MOVSXD, including REX.R/REX.B extended registers and correct sign extension from the source's low 32 bits into a 64-bit destination. Memory-source MOVSXD and the broader opcode family remain future work.


### v0.23.0 — MOVZX register forms

Added register-only MOVZX from 8-bit and 16-bit sources to 32-bit or 64-bit destinations. The implementation handles legacy AH/CH/DH/BH byte-register encodings when no REX prefix is present, and SPL/BPL/SIL/DIL plus extended byte registers when REX is present. Memory-source MOVZX remains unsupported.
