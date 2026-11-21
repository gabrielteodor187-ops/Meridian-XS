#include "meridian/cpu/cpu_state.hpp"

namespace meridian::cpu {

void CpuState::reset() noexcept {
    gpr.fill(0);
    rip = 0;
    rflags = 0x2;
    control_registers.fill(0);
    efer = 0;
    star = lstar = cstar = sfmask = 0;
    fs_base = gs_base = kernel_gs_base = 0;
    apic_base = tsc_aux = tsc = 0;
    sysenter_cs = sysenter_esp = sysenter_eip = 0;
    cs = ss = ds = es = fs = gs = 0;
    cpl = 0;
    x87_control_word = 0x037f;
    x87_status_word = x87_opcode = 0;
    x87_tag_word = 0;
    x87_instruction_pointer = x87_data_pointer = 0;
    mxcsr = 0x1f80;
    mxcsr_mask = 0x0000ffbf;
    for (auto& reg : st) reg.fill(std::byte{0});
    for (auto& reg : xmm) reg.fill(std::byte{0});
}

} // namespace meridian::cpu
