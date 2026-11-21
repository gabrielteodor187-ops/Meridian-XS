# Meridian-XS

An experimental, independently implemented research project exploring Xbox Series X|S emulation on PC.

> **Current status:** early CPU/memory foundations. This project does not boot Xbox software or run commercial games.

## Implemented

- C++23 / CMake project.
- Bounded guest-memory region with checked reads and writes.
- Little-endian typed memory access.
- x86-64 architectural CPU state container, including CR0–CR8 storage, EFER, and current privilege level.
- Initial instruction interpreter: NOP, HLT, and MOV register-immediate.
- Register-only 64-bit ADD, SUB, XOR, and CMP instructions.
- Short JMP and all 16 x86 condition-code branches, plus near 32-bit conditional branches.
- SETcc byte operations for all 16 condition codes, with register and supported memory destinations, including legacy high-byte and REX extended-byte register selection.
- Register-direct CMOVcc operations for all 16 condition codes, with 32-bit zero-extension and 64-bit operation sizes.
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
- Group-1 immediate arithmetic using imm32 and sign-extended imm8 encodings, with register and supported memory destinations.
- 32-bit and 64-bit memory-destination ADD, SUB, XOR, and CMP using base, disp8, disp32, SIB-scaled index, no-base SIB, and RIP-relative addressing.
- Page-granular virtual address space with explicit read/write/execute permissions, cross-page access, mapping, unmapping, atomic range protection changes, and typed little-endian read/write/instruction-fetch helpers.
- Four-level x86-64 page-table walker with 4 KiB and 2 MiB/1 GiB large-page translation, canonical-address checks, and RW/US/NX permission evaluation.
- CPU interpreter memory backend that fetches, reads, and writes through CR3 page-table translations, enforcing page permissions across page boundaries, with a small software TLB and explicit invalidation.
- Privileged MOV-to/from-CR0/CR2/CR3/CR4/CR8 register forms, INVLPG, SWAPGS, a limited RDMSR/WRMSR bank, basic SYSCALL/SYSRET transitions, conservative CPUID leaves, RDTSC, and RDTSCP.
- Read-only XVD/XVC header and descriptor metadata inspection, including region layout (no decryption or package extraction).
- Minimal standard PE32+ image mapping with AMD64 validation, section mapping, DIR64 base relocations, import-table metadata parsing, and an atomic callback-based IAT resolver (not Xbox executable support or built-in OS API emulation).
- Initial guest image mapper that allocates physical pages, constructs four-level page tables, applies PE section permissions, and maps a writable NX stack.
- Bounded guest execution engine with an explicit instruction budget and halt/fault/limit outcomes.
- XGETBV/XSETBV with XCR0 for x87/SSE/YMM state, plus standard-format XSAVE/XRSTOR support for x87, XMM, and YMM upper halves.
- FXSAVE/FXRSTOR support for a modeled 512-byte x87/SSE state image (including MXCSR and XMM state), plus LDMXCSR/STMXCSR with MXCSR_MASK validation.
- Scalar MOVD/MOVQ transfers between GPRs, memory, and XMM registers; MOVUPS/MOVUPD/MOVAPS/MOVAPD packed moves; MOVSS/MOVSD scalar moves with appropriate upper-lane behavior.
- Legacy CMPPS/CMPPD/CMPSS/CMPSD with all eight immediate predicate forms.
- Initial floating-point SIMD arithmetic: ADD/SUB/MUL/DIV, SQRT, MIN, and MAX variants across packed/scalar single/double forms with packed/scalar lane handling.
- Initial 128-bit SSE/SSE2 XMM operations: MOVDQA/MOVDQU, PXOR, PAND, POR, XORPS/XORPD, and packed byte/word/dword/qword ADD/SUB and EQ/GT comparisons, plus PMOVMSKB/MOVMSKPS/MOVMSKPD, AND/ANDN/OR/XOR packed bitwise operations, and PSHUFD/SHUFPS lane shuffles, with OSFXSR gating and MOVDQA alignment checks.
- CTest-based tests for memory, CPU state, XVD/XVC metadata, executable-image parsing, and supported instructions.
- GitHub Actions build-and-test workflow.

## Build and test

Requires CMake 3.24+ and a C++23 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Not implemented

This is not a usable Xbox emulator yet. Instruction coverage is tiny; there is no complete x86-64 decoder, complete CPU privilege/control-register/MSR/segment model, page-fault delivery, interrupt handling, Xbox kernel/system-service layer, XVC region reader, package decryption/authentication, GPU emulation, boot flow, or game compatibility. The CPU interpreter is an early educational foundation, not a commercial-game execution engine.

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


### v0.38.0 — CMOVcc register forms

Added the register-direct CMOVcc family for all 16 x86 condition codes. A true condition copies the source register; a false condition leaves the destination unchanged. REX.R/REX.B extended registers and 32-bit destination zero-extension are supported. Memory-source CMOVcc and 16-bit operand-size overrides remain unsupported. Tests cover true/false conditions and extended 64-bit registers.


### v0.39.0 — SETcc memory destinations

Extended SETcc to write byte results to guest memory using base/displacement, SIB, no-base SIB, and RIP-relative addressing. Added a shared checked effective-address decoder for this path. Memory writes use the normal backend, so bounds and virtual-page write permissions are enforced. Tests cover base-plus-disp8 and RIP-relative stores.


### v0.40.0 — Group-1 immediate memory operations

Extended Group-1 ADD, OR, AND, SUB, XOR, and CMP immediate operations to memory destinations for both 32-bit and 64-bit operand sizes. Both 0x81 imm32 and 0x83 sign-extended imm8 forms are supported, using base/displacement, SIB, no-base SIB, and RIP-relative effective addresses. RIP-relative calculations account for the trailing immediate bytes. Memory CMP remains non-writing, and stores use checked atomic memory writes. Tests cover 32-bit ADD, 64-bit AND, memory CMP, and RIP-relative imm8 addressing.


### v0.41.0 — x86-64 page-table translation foundation

Added a standalone four-level x86-64 page-table walker for 48-bit canonical virtual addresses. It handles 4 KiB pages and 2 MiB/1 GiB large-page entries, accumulates writable/user/NX permissions across levels, and models supervisor write bypass when CR0.WP is disabled. NXE handling and non-canonical address rejection are included. This walker is not yet connected to the CPU interpreter and does not implement TLBs, page-fault error codes, PCID, or the complete architectural reserved-bit checks.


### v0.41.1 — Page-table test include fix

Added the new page-table walker header to the core test translation unit and clarified the read-only page-entry mask expression. The page-walker implementation itself is unchanged.


### v0.42.0 — CPU execution through x86-64 page tables

Added X64PagedMemory, a virtual-memory backend that translates each CPU fetch/read/write through the four-level page tables rooted at CR3. The interpreter now has an overload for this backend, so instruction fetches honor NX and data writes honor page-level RW permissions. Range validation occurs before writes to prevent partial cross-page updates. End-to-end tests execute MOV, ADD, and HLT from a virtual code address while loading data from a separate virtual page, then verify NX fetch denial and read-only write denial. TLBs, page-fault error codes, CR0/CR4/EFER control-register state, and privilege transitions are still not implemented.


### v0.43.0 — Software TLB and invalidation

Added a small direct-mapped 64-entry translation cache to X64PagedMemory. Cached 4 KiB virtual-page translations retain the effective RW/US/NX permissions from the full page walk. Added INVLPG-style single-page invalidation, full TLB flush, and paging-context updates that flush when CR3 or permission mode changes. Tests verify stale cached mappings persist until invalidated and that invalidation exposes a remapped physical page. This is a simple functional TLB model; it does not model multi-core shootdowns, global pages, PCID, or hardware replacement policy.


### v0.43.1 — TLB permission-test invalidation

Updated the paging integration test to explicitly invalidate cached code/data translations after modifying their page-table entries. This reflects x86 TLB behavior: page-table memory writes do not automatically invalidate existing translations.


### v0.44.0 — Control registers and INVLPG

Extended CpuState with CR0–CR8 storage, EFER, and CPL. Added privileged MOV between GPRs and CR0/CR2/CR3/CR4/CR8, plus INVLPG for X64PagedMemory. The paged interpreter synchronizes CR3, CPL, CR0.WP, and EFER.NXE into the memory backend before and after each instruction; CR3 or paging-mode changes flush the software TLB. Added tests for CR3 switching, reading CR3, INVLPG remapping, and ring-3 privilege faults. Full control-register validation, MSR instructions, page-fault delivery, and privilege transitions are still incomplete.


### v0.44.1 — MOV-from-CR3 test encoding correction

Corrected the ModRM register encoding in the MOV RBX, CR3 test case. The interpreter implementation was unchanged; the original test byte sequence selected RCX as the destination.


### v0.45.0 — Modeled MSR access

Added a limited modeled MSR bank and privileged RDMSR/WRMSR execution. Supported indices include TSC, APIC_BASE, SYSENTER_CS/ESP/EIP, EFER, STAR/LSTAR/CSTAR/SFMASK, FS_BASE/GS_BASE/KERNEL_GS_BASE, and TSC_AUX. RDMSR splits values into EDX:EAX; WRMSR combines the low 32-bit halves. CPL violations return PrivilegeFault and unmodeled MSRs return GeneralProtectionFault. EFER writes automatically update the paging backend context after the instruction. This is not a complete AMD/Intel MSR implementation.


### v0.46.0 — SYSCALL/SYSRET and SWAPGS foundations

Added basic 64-bit SYSCALL and SYSRET transitions using EFER.SCE, STAR, LSTAR, SFMASK, RCX, and R11. The model updates CPL and CS/SS selectors and validates canonical target addresses. Added privileged SWAPGS behavior using GS_BASE and KERNEL_GS_BASE. Tests cover a user-to-kernel-to-user round trip, flag masking/restoration, selector changes, and SWAPGS privilege checks. Full segment descriptor tables, interrupt/exception delivery, IOPL, and complete architectural SYSRET validation remain unimplemented.


### v0.47.0 — CPUID and timestamp-counter instructions

Added CPUID leaves 0, 1, 0x80000000, and 0x80000001 with a deliberately conservative synthetic Meridian-XS vendor profile. Only currently modeled TSC, MSR, CMOV, long-mode, NX, and SYSCALL capabilities are advertised; SIMD and other unimplemented features are not claimed. Added deterministic RDTSC and RDTSCP counters, including TSC_AUX and CR4.TSD privilege checks. This CPUID profile is not an Xbox/AMD hardware-identification profile.


### v0.47.1 — CPUID output-register correction

Corrected CPUID result placement to the architectural EAX/EBX/ECX/EDX register mapping (RAX/RBX/RCX/RDX). Updated vendor-string and feature-bit assertions accordingly.


### v0.48.0 — Minimal PE32+ image loader

Added a standard AMD64 PE32+ image mapper. It validates DOS/PE headers, optional-header format, section table ranges, image bounds, and section overlap; maps headers and sections into a zero-initialized image; and applies DIR64 base relocations when loading away from the preferred image base. Tests use a synthetic PE image to verify section mapping, entry-point rebasing, relocation application, and malformed/non-AMD64 rejection. This is not an Xbox XEX/XVC package loader and does not resolve imports, process TLS callbacks, establish page tables, or launch an image.


### v0.49.0 — Map loaded PE images into guest memory

Added X64GuestImageMapper. It allocates guest physical pages, maps PE headers and sections at their virtual addresses, derives read/write/execute page permissions from section characteristics, builds four-level page tables, and maps an initial writable NX stack. Added an end-to-end test that loads a synthetic relocated PE32+ image, maps it, executes MOV EAX,42 and HLT through the page-table-backed CPU interpreter, and reads/writes the mapped stack. This is a generic PE test path, not Xbox XEX loading, kernel boot, or commercial-game compatibility.


### v0.50.0 — PE import metadata parsing

The PE32+ loader now parses the import directory after mapping and relocation. It enumerates imported module names and named or ordinal import symbols, validates descriptor/thunk termination and image bounds, and preserves hint/ordinal metadata for a future import resolver. It does not load DLLs, resolve symbols, patch the IAT, emulate Windows APIs, or support Xbox XEX containers. Synthetic fixture tests cover a named import.


### v0.50.1 — Ordinal import regression coverage

Expanded the synthetic PE fixture to include both a named import and an ordinal import. The test now verifies the ordinal flag and ordinal value are preserved by the import parser; removed a placeholder test that did not actually exercise ordinal parsing.


### v0.51.0 — Callback-based PE import resolution

Added an import resolver callback that receives each module and named/ordinal symbol. The loader records each IAT slot RVA and resolves all symbols before applying any patches, so a missing symbol leaves the image unchanged. Tests cover successful named/ordinal binding and atomic failure. This is an extension point for a future guest OS/API layer; no Windows or Xbox system libraries are implemented.


### v0.51.1 — Mutable import-resolution test fixture

Corrected the PE test fixture to retain a mutable loaded-image value while exercising IAT patching. No loader behavior changed.


### v0.52.0 — XVD/XVC header metadata inspection

Added a read-only parser for the public XVD header fields used by Xbox One-family and Series X|S XVC package metadata. It recognizes the `msft-xvd` magic and reports format version, volume flags, logical drive size, content type, volume type, declared metadata lengths, and block size. Tests cover XVC content classification, encryption/integrity flags, and malformed/truncated headers. It deliberately does not verify Microsoft signatures, decrypt package regions, access protected keys, or extract encrypted title files. Microsoft documents XVC as the Xbox console package format and notes that console package contents are encrypted; see https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/overviews/packaging?view=gdk-2604.


### v0.53.0 — XVC descriptor and region metadata

Extended the read-only XVD/XVC inspector to derive the XVC metadata offset from the declared embedded-volume, mutable-data, integrity-tree, and user-data page counts. It reads the descriptor version, initial-play region metadata, and region headers (IDs, flags, descriptions, offsets, and lengths), with strict bounds and count checks. Encryption key material is not read, and no region is decrypted or extracted. Layout research references the public xvdtool format notes; this implementation is independently written and limited to metadata inspection.


### v0.53.1 — XVD header fixture sizing

Expanded the header-only synthetic XVC fixture to include the metadata bytes declared by its header. The parser correctly rejected the earlier fixture because its declared XVC descriptor lay beyond the supplied file buffer; no parser behavior changed.


### v0.54.0 — Bounded guest execution engine

Added an execution loop over all three guest memory backends (bounded RAM, virtual address space, and CR3-backed paged memory). It stops on HLT, reports the first execution fault, or enforces a caller-supplied instruction budget. Tests cover a small program reaching HLT, an infinite guest loop stopped by the budget, and a zero budget. This provides a controlled execution harness; it does not add Xbox OS services or make commercial games compatible.


### v0.54.1 — PE image execution-engine integration test

The end-to-end synthetic PE test now runs the mapped entry point through the bounded execution engine instead of manually stepping the interpreter. It verifies that image parsing, page-table mapping, instruction execution, and HLT termination work together under an instruction budget.


### v0.55.0 — Initial SSE/SSE2 XMM instruction subset

Added 128-bit XMM register and memory support for MOVDQA/MOVDQU, PXOR, PAND, POR, XORPS, and XORPD. The first cut requires CR4.OSFXSR, rejects CR0.EM/TS rather than modeling their architectural exception delivery, and enforces 16-byte alignment for MOVDQA memory operands. Tests cover XMM register copies, unaligned-capable loads/stores, XOR/AND operations, OSFXSR gating, and alignment faults. This is only a small SIMD subset; no MXCSR, FXSAVE/FXRSTOR, packed arithmetic, SSE3/SSSE3/SSE4, AVX, or AVX2 support is claimed, and CPUID does not advertise these features yet.


### v0.56.0 — Packed SSE2 integer arithmetic

Added packed integer addition and subtraction for 8-, 16-, 32-, and 64-bit lanes: PADDB/PADDW/PADDD/PADDQ and PSUBB/PSUBW/PSUBD/PSUBQ. Each lane wraps independently at its element width and leaves integer flags unchanged. Tests verify independent 64-bit lane addition and subtraction. The SIMD backend remains an incomplete SSE/SSE2 subset and is not yet sufficient for commercial game workloads.


### v0.57.0 — Packed SSE2 comparisons and byte movemask

Added PCMPEQB/PCMPEQW/PCMPEQD and PCMPGTB/PCMPGTW/PCMPGTD, with signed comparisons for greater-than operations. Added register-form PMOVMSKB to gather the high bit of each XMM byte into a 32-bit GPR result. Tests cover alternating byte equality, movemask extraction, and signed dword comparisons. This remains an incomplete SIMD subset; floating-point SIMD, shuffles, scalar/vector conversions, and modern vector extensions are not yet implemented.


### v0.57.1 — Signed compare fixture correction

Corrected the first PCMPGTD test lane so the signed greater-than case compares positive 2 against positive 1. The earlier fixture compared -1 against 1 while expecting a true result. Interpreter behavior is unchanged.


### v0.58.0 — SSE2/SSE lane shuffles

Added PSHUFD (dword shuffle with imm8 lane selection) and SHUFPS (two-source dword shuffle). Memory-source effective-address decoding accounts for the trailing immediate byte, including RIP-relative addressing. Tests cover reverse-order PSHUFD selection and the identity SHUFPS control pattern. The SIMD implementation remains a deliberately incomplete subset.


### v0.58.1 — Memory-source shuffle coverage

Corrected the SIMD memory-source path to read operands for PSHUFD/SHUFPS. Added a RIP-relative PSHUFD test that verifies effective-address calculation includes the trailing immediate byte and that memory operands are actually loaded.


### v0.58.2 — Strict unsupported-prefix handling

Added a guard so legacy prefixes are not silently ignored by unrelated two-byte opcodes. A regression test verifies that an operand-size-prefixed near conditional branch is rejected rather than incorrectly executed as a 32-bit branch.


### v0.58.3 — Shuffle fixture memory bounds

Expanded the PSHUFD/SHUFPS test's guest-memory fixture so its RIP-relative instruction and source vector fit within the allocated region. The previous fixture ended exactly where the new instruction was written.


### v0.59.0 — FXSAVE/FXRSTOR state management

Added 64-bit-mode FXSAVE/FXRSTOR memory forms with 16-byte alignment checks, CR4.OSFXSR gating, a 512-byte state image, MXCSR reserved-bit validation, XMM register save/restore, and opaque x87 register-state preservation. Added modeled x87 control/status/tag/opcode and instruction/data pointer fields plus MXCSR defaults to CpuState. Tests cover state round-trip, alignment rejection, and invalid MXCSR rejection. This does not implement x87 arithmetic or the full FPU exception model, and CPUID does not yet advertise the broader SIMD feature set.


### v0.60.0 — MOVD/MOVQ scalar XMM transfers

Added 32-bit MOVD and REX.W 64-bit MOVQ transfer forms between general-purpose registers or memory and XMM registers. Loads zero the unused upper XMM bits; 32-bit GPR destinations zero-extend. Tests cover register transfers and a memory-source MOVD. This does not add the separate F3-prefixed XMM-to-XMM MOVQ forms.


### v0.60.1 — Prefix dispatch ordering

Moved the strict unsupported-prefix guard after the explicitly supported MOVD/MOVQ and FXSAVE/FXRSTOR handlers. This allows 66-prefixed scalar XMM transfer instructions to reach their decoder while still rejecting unsupported prefixed two-byte opcodes.


### v0.60.2 — MOVQ register test encoding

Corrected the MOVQ XMM1-to-RCX test's ModRM byte so it selects XMM1 as the source register. The implementation was unchanged.


### v0.61.0 — Packed and scalar SSE move families

Added MOVUPS/MOVUPD and aligned MOVAPS/MOVAPD 128-bit moves, plus MOVSS/MOVSD scalar loads and stores. Register-source scalar moves preserve upper destination bits; scalar memory loads zero upper XMM bits; scalar stores write only 32 or 64 bits. Aligned packed memory forms enforce 16-byte alignment. Tests cover packed copies, unaligned loads, scalar upper-lane semantics, scalar stores, and alignment faults. This remains a partial SSE implementation.


### v0.62.0 — Basic packed and scalar floating-point SIMD arithmetic

Added ADD/SUB/MUL/DIV for packed single/double and scalar single/double forms (PS/PD/SS/SD). Packed operations update all lanes; scalar operations update only the low lane and preserve the remaining destination bits. Tests cover ADDPS, ADDSS, and ADDPD. This initial implementation uses the host's default IEEE floating-point environment; guest MXCSR rounding modes, exception masks/status flags, denormal controls, and precise SIMD exception delivery are not yet modeled.


### v0.62.1 — ADDSS test input isolation

Corrected the ADDSS test to use the original left-hand vector rather than the already modified ADDPS result. The test now checks the intended 1.0 + 10.0 scalar case and compares preserved upper lanes against the original destination state.


### v0.63.0 — SSE packed bitwise operations and sign masks

Added ANDPS/ANDPD, ANDNPS/ANDNPD, ORPS/ORPD alongside the existing XOR operations, and MOVMSKPS/MOVMSKPD to extract packed lane sign bits into a GPR. Tests cover ANDNPS semantics and 32-/64-bit movemask extraction. Floating-point exception and MXCSR behavior remain incomplete.


### v0.64.0 — LDMXCSR/STMXCSR

Added memory-form LDMXCSR and STMXCSR to load/store the guest MXCSR register. LDMXCSR rejects reserved bits according to the modeled MXCSR_MASK and leaves state unchanged on failure. Tests cover valid load/store and invalid reserved-bit rejection. Floating-point SIMD operations still use the host's default rounding behavior and do not yet update guest MXCSR exception flags.


### v0.64.1 — MOVMSKPD test register selection

Corrected the MOVMSKPD test's ModRM byte to select RAX as the destination, matching the assertion. The instruction implementation was unchanged.


### v0.65.0 — SSE floating-point compare predicates

Added CMPPS/CMPPD/CMPSS/CMPSD with the eight legacy immediate predicates (ordered equality/less/less-equal, unordered, not-equal, not-less, not-less-equal, and ordered). Packed comparisons update every lane; scalar comparisons update only the low lane. Tests cover ordered equality, NaN/unordered handling, and scalar upper-lane preservation. Signaling-vs-quiet exception behavior and MXCSR exception flags are not yet modeled.


### v0.66.0 — SSE square root and min/max

Added SQRTPS/SQRTPD/SQRTSS/SQRTSD and MIN/MAX packed/scalar single/double forms. Min/max follow the legacy SSE selection rule of choosing the source operand when values compare equal or unordered. Tests cover packed square roots and scalar minimum with upper-lane preservation. Guest MXCSR exception flags and rounding controls remain incomplete.


### v0.67.0 — XGETBV/XSETBV and XCR0 foundation

Added XGETBV and XSETBV for XCR0 index 0, including CR4.OSXSAVE checks, XSETBV privilege checks, and validation of x87/SSE enable dependencies. XCR0 resets to x87-only and can enable the currently modeled SSE state. AVX/YMM state is explicitly rejected until extended vector-state storage and execution are implemented. Tests cover read/write, privilege, feature gating, and rejection of unavailable AVX state.


### v0.68.0 — XSAVE/XRSTOR and YMM upper-half state

Added 256-bit YMM state storage and standard-format XSAVE/XRSTOR for the currently modeled x87, SSE, and YMM upper-half components. XCR0 can now enable bits 0-2 with architectural dependency checks. XSAVE/XRSTOR require a 64-byte-aligned area and, in this first implementation, the request mask must exactly match XCR0; compacted format and partial component masks are not supported. CPUID now exposes XSAVE/OSXSAVE and leaf 0xD sizing for the modeled state components. Initial AVX VEX.0F support: 128-/256-bit VADDPS, VXORPS, VMOVUPS/VMOVAPS, VZEROUPPER, and VZEROALL. An initial AVX2 integer subset: VPADDQ/VPADDD, VPSUBQ/VPSUBD, VPAND/VPOR/VPXOR, and 256-bit YMM integer lanes.


### v0.68.1 — CPUID xstate feature tests

Updated the basic-leaf maximum assertion to include leaf 0xD and added tests for XSAVE/OSXSAVE feature reporting and the leaf 0xD supported/enabled state sizes. AVX remains unadvertised until VEX instruction execution is implemented.


### v0.69.0 — Initial AVX VEX instruction subset

Added two-byte and three-byte VEX prefix decoding for the 0F opcode map and initial 128-/256-bit AVX instructions: VADDPS/VADDPD, VSUBPS/PD, VMULPS/PD, VDIVPS/PD, VANDPS/PD, VANDNPS/PD, VORPS/PD, VXORPS/PD, VMOVUPS/UPD and VMOVAPS/APD, VZEROUPPER, and VZEROALL. VEX.128 writes clear the destination's upper YMM half; VEX.256 operations process all eight single-precision lanes. Execution requires OSXSAVE, OSFXSR, and XCR0 x87/SSE/AVX state enablement. Tests cover 128-/256-bit VADDPS and zero-upper behavior. This is not a complete AVX implementation, and AVX2 remains unsupported and unadvertised.


### v0.70.0 — AVX double-precision and bitwise forms

Extended the VEX.0F subset with packed double-precision arithmetic (VADDPD/VSUBPD/VMULPD/VDIVPD), packed bitwise AND/ANDN/OR/XOR forms for PS and PD, and 128-/256-bit unaligned/aligned packed double move encodings. Tests cover VADDPD and VANDNPS. This remains a subset of AVX; AVX2 and many AVX instructions are still missing, and CPUID continues to avoid advertising AVX prematurely.


### v0.71.0 — Initial AVX2 packed integer subset

Added VEX.66 integer vector operations for VPADDQ/VPADDD, VPSUBQ/VPSUBD, VPAND/VPOR/VPXOR, and 256-bit YMM lanes. Added VMOVDQA/VMOVDQU encodings to the VEX move decoder with aligned/unaligned memory behavior. Tests cover 256-bit VPADDQ and VPXOR. This is only a small AVX2 subset; CPUID does not yet advertise AVX2.
