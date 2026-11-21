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
- Short JMP and all 16 x86 condition-code branches, plus near 32-bit conditional branches.
- Register-direct SETcc byte operations for all 16 condition codes, including legacy high-byte and REX extended-byte register selection.
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
- Register and memory destination 32-bit/64-bit OR and AND operations, with logical flag updates.
- Register-direct Group-1 immediate arithmetic using both imm32 and sign-extended imm8 encodings.
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


### v0.24.0 — MOVSX register forms

Added register-only MOVSX from signed 8-bit and 16-bit sources to 32-bit or 64-bit destinations. It shares the correct legacy high-byte versus REX byte-register selection with MOVZX, and tests signed byte/word extension, 32-bit zero-extension, and extended registers. Memory-source MOVSX remains unsupported.


### v0.25.0 — C7 immediate MOV register forms

Added the register-direct C7 /0 forms: 32-bit destinations receive imm32 with zero-extension, while 64-bit destinations receive a sign-extended imm32. REX.B selects R8–R15. Memory-destination C7 forms are not yet implemented.


### v0.26.0 — C7 immediate MOV memory forms

Extended C7 /0 to memory destinations, including base-plus-displacement and SIB scaled-index addressing. Both 32-bit imm32 stores and 64-bit sign-extended imm32 stores are supported. Instruction immediates are fetched through the execute path and data writes use normal guest-memory permissions/bounds checks. RIP-relative and full x86 addressing coverage remain incomplete for this opcode.


### v0.27.0 — RIP-relative C7 immediate MOV

Added RIP-relative addressing for C7 memory destinations. The effective address is calculated from the instruction pointer after the complete instruction, including both displacement and immediate fields. An automated test verifies the exact target address and stored value.


### v0.28.0 — C7 virtual-memory permission coverage

Added an end-to-end test for C7 immediate stores through the CPU's virtual-address-space overload. A write to a read-only mapped data page must return MemoryFault, leave RIP unchanged, and preserve backing memory. This validates the new immediate-store path against the same page permissions used by ordinary CPU memory operations.


### v0.29.0 — Atomic cross-page immediate-store validation

Added a regression test for a 64-bit C7 store that crosses from a writable virtual page into a read-only page. The operation must fault before changing either page, proving that the virtual-address-space write validation prevents partial stores on permission failures.


### v0.30.0 — Memory-source MOVSX/MOVZX

Extended MOVSX and MOVZX byte/word forms to memory operands using base/displacement, SIB scaled-index, and RIP-relative addressing. Loads use the memory read path, so virtual-page read permissions are enforced. Register forms, including legacy high-byte and REX byte registers, remain supported. Full decoder and address-size coverage are still incomplete.


### v0.31.0 — Memory-source MOVSXD

Extended MOVSXD to read 32-bit operands from guest memory using base/displacement, SIB scaled-index, and RIP-relative forms. The result is sign-extended to 64 bits with REX.W, or sign-extended to 32 bits and then zero-extended into the GPR without REX.W. Memory reads honor the active memory backend's access checks.


### v0.32.0 — MOVZX virtual read-permission coverage

Added a CPU/virtual-address-space test proving that a memory-source MOVZX faults when its source page is mapped write-only. The test also verifies that RIP remains unchanged, confirming that these new load forms use the checked data-read path rather than instruction-fetch access.


### v0.33.0 — Immediate AND/OR register operations

Extended the 0x81 immediate arithmetic group with 32-bit and 64-bit register-direct OR and AND operations. Results use the existing logical-flag behavior, clearing CF and OF while updating ZF/SF/PF. Memory-destination forms of these operations are not yet supported.


### v0.34.0 — Register and memory OR/AND operations

Added the x86-64 OR r/m, r and AND r/m, r instruction forms for 32-bit and 64-bit operands. Both register-direct and supported memory destinations are handled; memory forms reuse the existing base, SIB, displacement, and RIP-relative address calculation. Logical flags are updated and memory writes go through the checked guest-memory backend. Automated tests cover 32-bit register OR, 64-bit register AND, and 32/64-bit memory destinations.


### v0.34.1 — OR/AND register-form correction

Completed the register-direct execution branches for the OR and AND opcodes and corrected the memory-OR regression test's expected bitwise result. The v0.34.0 implementation already included the memory-destination paths; this patch makes the documented register and memory coverage consistent.


### v0.35.0 — Sign-extended imm8 arithmetic

Added register-direct Group-1 opcode 0x83 for ADD, OR, AND, SUB, XOR, and CMP. The signed 8-bit immediate is extended to the active operand width, including 64-bit operations, and existing arithmetic/logical flag helpers are reused. Tests cover negative imm8 values, 32-bit zero-extension, 64-bit sign-extension, and REX.B extended-register selection. Memory-destination forms of opcode 0x83 remain unsupported.


### v0.36.0 — Complete condition-code branch family

Expanded short conditional branches to all 16 x86 condition codes and added the corresponding near rel32 forms. Conditions are evaluated from CF, PF, ZF, SF, and OF, including unsigned and signed comparisons. Tests exercise the full short-condition family and a near JB branch. This remains a partial CPU implementation; branch-target canonicality and full exception semantics are not modeled.


### v0.37.0 — SETcc register forms

Added all 16 SETcc condition-code operations for register byte destinations. The implementation preserves the other bits in the destination GPR, supports AH/CH/DH/BH without a REX prefix, and selects SPL/BPL/SIL/DIL or R8B–R15B when REX is present. Memory destinations are not yet supported. Tests cover SETZ, SETNZ, SETB, high-byte preservation, and REX.B extension.


### v0.37.1 — Conditional branch test diagnostics

Improved the short-condition regression test failure output to identify the exact opcode and flag combination if a branch condition is evaluated incorrectly.


### v0.37.2 — Branch test layout correction

Corrected the conditional-branch test fixture so the displacement and branch body are written after the opcode byte rather than over it. Kept opcode/flag diagnostics for future regressions. No interpreter behavior changed in this patch.
