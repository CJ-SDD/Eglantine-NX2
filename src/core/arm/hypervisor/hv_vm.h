// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "common/host_memory.h"
#include "core/hardware_properties.h"

namespace Common {
struct PageTable;
}

namespace Core::Hv {

/// EDEN_NX2_HYPERVISOR=1: run application code natively on Apple silicon through
/// Hypervisor.framework instead of translating it with Dynarmic.
bool IsRequested();

/**
 * The process-wide virtual machine that application code runs in.
 *
 * Guest physical memory (the IPA space) holds the emulated DRAM at its Switch physical addresses
 * (DramMemoryMap::Base and up), backed by the DeviceMemory buffer, and a pool of host pages for
 * the stage-1 page tables and the EL1 exception stub. The stage-1 tables use a 4 KiB granule, so
 * guest pages map one to one regardless of the host's 16 KiB pages: they mirror the emulated
 * process page table, which Core::Memory reports here as it changes.
 */
class Vm {
public:
    static Vm& Get();

    /// Attaches the application page table: (re)creates the VM if needed, maps the DRAM backing
    /// and starts an empty set of stage-1 tables. Returns false if the VM can't be used.
    bool Attach(Common::PageTable& table, size_t address_space_bits, u8* backing,
                size_t backing_size);
    bool IsAttached(const Common::PageTable& table) const {
        return attached_table == &table;
    }
    bool IsReady() const {
        return ready.load(std::memory_order_acquire);
    }

    // Mirror of the process page table. Addresses and sizes are 4 KiB aligned.
    void Map(u64 vaddr, u64 size, u64 paddr, Common::MemoryPermission perms);
    void Unmap(u64 vaddr, u64 size);
    void Protect(u64 vaddr, u64 size, Common::MemoryPermission perms);
    /// Pages the GPU caches track: writes (and reads, with reactive flushing) fault so the
    /// caches see them, as fastmem's protections do.
    void MarkCached(u64 vaddr, u64 size, bool cached, bool block_reads);

    struct PageState {
        bool mapped;
        bool readable;
        bool writable;
        bool executable;
        bool cached;
        u64 paddr;
    };
    PageState Query(u64 vaddr) const;

    /// Makes host writes to guest code visible to instruction fetches.
    void SyncInstructionCache(u64 vaddr, u64 size);
    /// vCPUs invalidate their instruction caches before running again.
    void InvalidateAllCode() {
        code_generation.fetch_add(1, std::memory_order_acq_rel);
    }

    // Register values for the vCPUs.
    u64 Ttbr0() const {
        return ttbr0_ipa;
    }
    u64 Ttbr1() const {
        return ttbr1_ipa;
    }
    u64 Tcr() const;
    static constexpr u64 StubVa = 0xFFFF'FFFF'FFFF'F000ULL; ///< Vector table, then entry code.
    static constexpr u64 EntryFlushVa = StubVa + 0x800;    ///< TLB and I-cache flush, then ERET.
    static constexpr u64 EntryPlainVa = StubVa + 0x820;    ///< ERET.

    /// Bumped when mappings lose permissions: vCPUs flush their TLBs before running again.
    u64 TlbGeneration() const {
        return tlb_generation.load(std::memory_order_acquire);
    }
    u64 CodeGeneration() const {
        return code_generation.load(std::memory_order_acquire);
    }

    // Running vCPUs, so permission losses can make them flush.
    void SetRunning(size_t core, u64 vcpu);
    void ClearRunning(size_t core);

private:
    Vm();

    bool CreateVm();
    u64* TableFor(u64 vaddr, bool create);
    u64 AllocateTable();
    u64* HostPointer(u64 ipa) const;
    void StoreEntry(u64* entry, u64 value);
    static u64 Descriptor(u64 paddr, u64 software);
    void Rebuild(u64* entry);
    void KickRunning();

    mutable std::mutex mutex;
    std::atomic<bool> ready{};
    bool vm_created{};
    Common::PageTable* attached_table{};
    size_t as_bits{};
    u32 ipa_bits{36};

    u8* mapped_backing{};
    size_t mapped_backing_size{};

    // Page-table pool: host memory mapped at PoolIpa.
    u8* pool{};
    size_t pool_used{};
    u64 ttbr0_ipa{};
    u64 ttbr1_ipa{};

    std::atomic<u64> tlb_generation{1};
    std::atomic<u64> code_generation{1};
    std::array<std::atomic<u64>, Core::Hardware::NUM_CPU_CORES> running{};
};

} // namespace Core::Hv
