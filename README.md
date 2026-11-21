# Meridian-XS

An experimental, independently implemented research project exploring Xbox Series X|S emulation on PC.

> **Current status:** early CPU/memory foundations. This project does not boot Xbox software or run commercial games.

## Implemented

- C++23 / CMake project.
- Bounded guest-memory region with checked reads and writes.
- Little-endian typed memory access.
- x86-64 architectural CPU state container.
- Initial instruction interpreter: NOP, HLT, and MOV register-immediate (including REX.B/REX.W forms).
- CTest-based tests for memory, CPU state, and supported instructions.

## Build and test

Requires CMake 3.24+ and a C++23 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Not implemented

This is not a usable Xbox emulator yet. Instruction coverage is tiny; there is no complete x86-64 decoder, privilege model, virtual memory, interrupt handling, Xbox kernel/system-service layer, GPU emulation, boot flow, or game compatibility. The CPU interpreter is an early educational foundation, not a commercial-game execution engine.

## Development direction

1. Expand x86-64 decoding and implement arithmetic, branches, stack, and memory instructions with tests.
2. Build a mapped address-space model with page permissions.
3. Add interrupts, scheduling, and system-call/platform abstractions.
4. Research graphics command translation and console-specific interfaces using public documentation.

All implementation is intended to be original code based on public information. No proprietary SDKs, firmware, keys, or game data are included.
