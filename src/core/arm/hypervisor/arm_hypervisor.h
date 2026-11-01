// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "core/arm/arm_interface.h"

namespace Kernel {
class KProcess;
}

namespace Core {

class System;

/**
 * Runs AArch64 application code natively on Apple silicon inside a Hypervisor.framework VM.
 *
 * Guest code runs at EL0 with the stage-1 tables of Hv::Vm. Every exception it takes goes to a
 * small EL1 stub that issues HVC, which exits to the host here: SVCs return to the kernel as with
 * the other backends, counter reads are emulated at the guest's frequency, and faults on pages the
 * GPU caches protect are resolved like fastmem's. Each host thread owns one vCPU.
 */
class ArmHypervisor final : public ArmInterface {
public:
    ArmHypervisor(System& system, Kernel::KProcess* process, std::size_t core_index);
    ~ArmHypervisor() override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }
    void RewindBreakpointInstruction() override {}

private:
    struct Context {
        std::array<u64, 31> x{};
        u64 sp{};
        u64 pc{};
        u32 pstate{};
        std::array<u128, 32> v{};
        u32 fpcr{};
        u32 fpsr{};
        u64 tpidr_el0{};
        u64 tpidrro_el0{};
    };

    struct Vcpu;
    Vcpu& CurrentVcpu();
    void WriteState(Vcpu& vcpu);
    void ReadState(Vcpu& vcpu);
    enum class Resolution { Continue, Return };
    Resolution HandleLowerSync(Vcpu& vcpu, u64 esr, u64 far, u64 elr, u64 spsr, HaltReason& out);
    Resolution HandleDataAbort(Vcpu& vcpu, u64 esr, u64 far, u64 elr);
    void EmulateSystemRegister(Vcpu& vcpu, u64 esr);

    System& m_system;
    Kernel::KProcess* m_process;
    std::size_t m_core_index;

    Context m_ctx{};
    u32 m_svc{};

    std::atomic<bool> m_break{};
    std::atomic<bool> m_running{};
    std::atomic<u64> m_running_vcpu{~u64{0}};

    // Exit statistics, logged every 10 seconds per core (EDEN_NX2_HYPERVISOR_STATS=1).
    struct Stats {
        u64 runs{}, svc{}, counter{}, data_abort{}, interrupt{}, cancel{}, vtimer{}, other{};
        u64 native_ns{};
        s64 window_start_ns{};
    };
    void CountAndReport(u64 run_ns);
    Stats m_stats{};
    bool m_stats_enabled{};

    u64 m_last_fault_page{~u64{0}};
    u32 m_fault_repeats{};
};

} // namespace Core
