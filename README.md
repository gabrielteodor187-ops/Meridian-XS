# Meridian-XS

An experimental, independently implemented research project exploring Xbox Series X|S emulation on PC.

> **Current status:** early foundations only. This project does not boot Xbox software or run commercial games.

## Implemented

- C++23 / CMake project.
- Bounded guest-memory region with checked reads and writes.
- Little-endian typed memory access for unsigned integer types.
- x86-64 architectural CPU state container (general-purpose registers, RIP, RFLAGS, and XMM register storage).
- CTest-based core test executable.

## Build

Requires CMake 3.24+ and a C++23 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Not implemented

There is no instruction decoder, CPU interpreter/JIT, Xbox kernel or system-service layer, GPU emulation, boot chain, or game compatibility yet. The CPU state structure is only a data model; it does not execute x86-64 instructions.

## Development direction

1. Expand guest memory into a mapped address-space model with permissions and regions.
2. Add x86-64 instruction decoding and a tested interpreter for a deliberately limited instruction subset.
3. Define platform abstractions for scheduling, interrupts, and system calls.
4. Research graphics command translation and the console-specific system interfaces using public documentation.

All implementation is intended to be original code based on public information. No proprietary SDKs, firmware, keys, or game data are included.
