// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/device_memory.h"

namespace Core::Hv {

namespace {

constexpr u64 PageSize = 0x1000;
constexpr u64 PageMask = PageSize - 1;
constexpr u64 HostPageSize = 0x4000;

// IPA layout: emulated DRAM at its physical addresses, the table pool above it.
constexpr u64 PoolIpa = 0x8'0000'0000ULL; // 32 GiB
constexpr size_t PoolSize = 512ULL << 20;

// Descriptor fields (4 KiB granule).
constexpr u64 DescValid = 0b11;          // table (levels 0-2) or page (level 3)
constexpr u64 DescAddressMask = 0x0000'FFFF'FFFF'F000ULL;
constexpr u64 DescApShift = 6;
constexpr u64 DescShInner = 0b11ULL << 8;
constexpr u64 DescAf = 1ULL << 10;
constexpr u64 DescPxn = 1ULL << 53;
constexpr u64 DescUxn = 1ULL << 54;
// Software bits (ignored by the MMU): the kernel's permissions and the GPU-cache protections, so
// the effective permissions can be recomputed when either changes.
constexpr u64 SwRead = 1ULL << 55;
constexpr u64 SwWrite = 1ULL << 56;
constexpr u64 SwExecute = 1ULL << 57;
constexpr u64 SwCachedNoWrite = 1ULL << 58;
constexpr u64 SwCachedNoRead = 1ULL << 59;
constexpr u64 SwMask = SwRead | SwWrite | SwExecute | SwCachedNoWrite | SwCachedNoRead;

u64 SoftwareFor(Common::MemoryPermission perms) {
    u64 sw = 0;
    if (True(perms & Common::MemoryPermission::Read)) sw |= SwRead;
    if (True(perms & Common::MemoryPermission::Write)) sw |= SwWrite;
    if (True(perms & Common::MemoryPermission::Execute)) sw |= SwExecute;
    return sw;
}

// Exception stub instructions.
constexpr u32 InsnHvc(u32 imm) {
    return 0xD4000002U | (imm << 5);
}
constexpr u32 InsnBranchSelf = 0x14000000U;
constexpr u32 InsnTlbiVmalle1is = 0xD508831FU;
constexpr u32 InsnDsbIsh = 0xD5033B9FU;
constexpr u32 InsnIcIalluis = 0xD508711FU;
constexpr u32 InsnIsb = 0xD5033FDFU;
constexpr u32 InsnEret = 0xD69F03E0U;

u32 IpsFor(u32 bits) {
    if (bits <= 32) return 0;
    if (bits <= 36) return 1;
    if (bits <= 40) return 2;
    if (bits <= 42) return 3;
    if (bits <= 44) return 4;
    return 5;
}

} // namespace

bool IsRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_HYPERVISOR");
        return value != nullptr && value[0] != '0';
    }();
    return requested;
}

Vm::Vm() {
    for (auto& slot : running) {
        slot.store(~u64{0}, std::memory_order_relaxed);
    }
}

Vm& Vm::Get() {
    static Vm vm;
    return vm;
}

bool Vm::CreateVm() {
    if (vm_created) {
        return true;
    }
    hv_vm_config_t config = hv_vm_config_create();
    uint32_t max_ipa = 0;
    if (hv_vm_config_get_max_ipa_size(&max_ipa) == HV_SUCCESS && max_ipa >= 36) {
        ipa_bits = std::min<uint32_t>(max_ipa, 40);
        hv_vm_config_set_ipa_size(config, ipa_bits);
    }
    const hv_return_t result = hv_vm_create(config);
    os_release(config);
    if (result != HV_SUCCESS) {
        LOG_CRITICAL(Core_ARM,
                     "Hypervisor: hv_vm_create failed ({:#x}). The app needs the "
                     "com.apple.security.hypervisor entitlement; falling back to Dynarmic",
                     static_cast<u32>(result));
        return false;
    }

    pool = static_cast<u8*>(
        mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (pool == MAP_FAILED) {
        pool = nullptr;
        LOG_CRITICAL(Core_ARM, "Hypervisor: page table pool allocation failed");
        return false;
    }
    if (hv_vm_map(pool, PoolIpa, PoolSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) !=
        HV_SUCCESS) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: mapping the page table pool failed");
        return false;
    }
    vm_created = true;
    LOG_INFO(Core_ARM, "Hypervisor: VM created, {}-bit guest physical addresses", ipa_bits);
    return true;
}

bool Vm::Attach(Common::PageTable& table, size_t address_space_bits, u8* backing,
                size_t backing_size) {
    std::scoped_lock lk{mutex};
    ready.store(false, std::memory_order_release);
    attached_table = nullptr;

    if (address_space_bits > 48 || address_space_bits < 32) {
        LOG_ERROR(Core_ARM, "Hypervisor: unsupported {}-bit address space", address_space_bits);
        return false;
    }
    if (!CreateVm()) {
        return false;
    }

    // Guest DRAM.
    backing_size = Common::AlignDown(backing_size, HostPageSize);
    if (mapped_backing != backing || mapped_backing_size != backing_size) {
        if (mapped_backing != nullptr) {
            hv_vm_unmap(DramMemoryMap::Base, mapped_backing_size);
            mapped_backing = nullptr;
        }
        const hv_return_t result =
            hv_vm_map(backing, DramMemoryMap::Base, backing_size,
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        if (result != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: mapping {:#x} bytes of guest DRAM failed ({:#x})",
                         backing_size, static_cast<u32>(result));
            return false;
        }
        mapped_backing = backing;
        mapped_backing_size = backing_size;
    }

    // Fresh tables: page 0 is the stub, then the TTBR0 root and the TTBR1 tables.
    std::memset(pool, 0, pool_used);
    pool_used = 0;
    const u64 stub_ipa = AllocateTable();
    ttbr0_ipa = AllocateTable();
    ttbr1_ipa = AllocateTable();
    as_bits = address_space_bits;

    // TTBR1 (T1SZ = 25, starting at level 1) maps only the stub page, at the top of the address
    // space: EL1 read/execute, never EL0.
    {
        u64* l1 = HostPointer(ttbr1_ipa);
        const u64 l2_ipa = AllocateTable();
        const u64 l3_ipa = AllocateTable();
        l1[511] = l2_ipa | DescValid;
        HostPointer(l2_ipa)[511] = l3_ipa | DescValid;
        HostPointer(l3_ipa)[511] = (stub_ipa & DescAddressMask) | DescValid | (0b10ULL << DescApShift) |
                                   DescShInner | DescAf | DescUxn;
    }

    // The stub: vectors that hand every exception to the host, then the entry sequences.
    u32* stub = reinterpret_cast<u32*>(HostPointer(stub_ipa));
    for (u32 vector = 0; vector < 16; ++vector) {
        stub[vector * 0x20] = InsnHvc(vector);
        stub[vector * 0x20 + 1] = InsnBranchSelf;
    }
    const std::array<u32, 6> flush{InsnTlbiVmalle1is, InsnDsbIsh, InsnIcIalluis,
                                   InsnDsbIsh,        InsnIsb,    InsnEret};
    std::memcpy(&stub[0x800 / 4], flush.data(), sizeof(flush));
    stub[0x820 / 4] = InsnEret;
    sys_icache_invalidate(stub, PageSize);

    attached_table = &table;
    tlb_generation.fetch_add(1, std::memory_order_acq_rel);
    code_generation.fetch_add(1, std::memory_order_acq_rel);
    ready.store(true, std::memory_order_release);
    LOG_INFO(Core_ARM, "Hypervisor: attached a {}-bit address space, {:#x} bytes of DRAM",
             address_space_bits, backing_size);
    return true;
}

u64 Vm::Tcr() const {
    const u64 t0sz = 64 - as_bits;
    const u64 t1sz = 25;
    u64 tcr = 0;
    tcr |= t0sz;                 // T0SZ
    tcr |= 0b01ULL << 8;         // IRGN0: write-back, write-allocate
    tcr |= 0b01ULL << 10;        // ORGN0
    tcr |= 0b11ULL << 12;        // SH0: inner shareable
    tcr |= 0b00ULL << 14;        // TG0: 4 KiB
    tcr |= t1sz << 16;           // T1SZ
    tcr |= 0b01ULL << 24;        // IRGN1
    tcr |= 0b01ULL << 26;        // ORGN1
    tcr |= 0b11ULL << 28;        // SH1
    tcr |= 0b10ULL << 30;        // TG1: 4 KiB
    tcr |= u64{IpsFor(ipa_bits)} << 32; // IPS
    return tcr;
}

u64* Vm::HostPointer(u64 ipa) const {
    return reinterpret_cast<u64*>(pool + (ipa - PoolIpa));
}

u64 Vm::AllocateTable() {
    if (pool_used + PageSize > PoolSize) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: out of page table memory");
        return 0;
    }
    const u64 ipa = PoolIpa + pool_used;
    pool_used += PageSize;
    return ipa;
}

void Vm::StoreEntry(u64* entry, u64 value) {
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
}

u64* Vm::TableFor(u64 vaddr, bool create) {
    // Walk from the starting level down to the level 3 table holding vaddr's entry.
    const u32 start_level = as_bits > 39 ? 0 : (as_bits > 30 ? 1 : 2);
    u64* table = HostPointer(ttbr0_ipa);
    for (u32 level = start_level; level < 3; ++level) {
        const u32 shift = 39 - level * 9;
        u64* entry = &table[(vaddr >> shift) & 511];
        u64 value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if ((value & DescValid) != DescValid) {
            if (!create) {
                return nullptr;
            }
            const u64 next = AllocateTable();
            if (next == 0) {
                return nullptr;
            }
            value = next | DescValid;
            StoreEntry(entry, value);
        }
        table = HostPointer(value & DescAddressMask);
    }
    return &table[(vaddr >> 12) & 511];
}

u64 Vm::Descriptor(u64 paddr, u64 software) {
    const bool readable = (software & SwRead) && !(software & SwCachedNoRead);
    const bool writable = readable && (software & SwWrite) && !(software & SwCachedNoWrite);
    // AP: 01 = EL0 read/write, 11 = EL0 read-only, 00 = no EL0 access.
    const u64 ap = readable ? (writable ? 0b01 : 0b11) : 0b00;
    u64 value = (paddr & DescAddressMask) | DescValid | (ap << DescApShift) | DescShInner | DescAf |
                DescPxn | (software & SwMask);
    if (!(software & SwExecute)) {
        value |= DescUxn;
    }
    return value;
}

void Vm::KickRunning() {
    for (auto& slot : running) {
        u64 vcpu = slot.load(std::memory_order_acquire);
        if (vcpu != ~u64{0}) {
            hv_vcpu_t id = static_cast<hv_vcpu_t>(vcpu);
            hv_vcpus_exit(&id, 1);
        }
    }
}

void Vm::SetRunning(size_t core, u64 vcpu) {
    running[core].store(vcpu, std::memory_order_seq_cst);
}

void Vm::ClearRunning(size_t core) {
    running[core].store(~u64{0}, std::memory_order_release);
}

void Vm::Map(u64 vaddr, u64 size, u64 paddr, Common::MemoryPermission perms) {
    bool lost = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        const u64 software = SoftwareFor(perms);
        for (u64 offset = 0; offset < size; offset += PageSize) {
            u64* entry = TableFor(vaddr + offset, true);
            if (entry == nullptr) {
                return;
            }
            const u64 old = *entry;
            const u64 value = Descriptor(paddr + offset, software);
            if ((old & DescValid) == DescValid &&
                ((old & DescAddressMask) != (value & DescAddressMask) || (old & ~value & SwMask))) {
                lost = true;
            }
            StoreEntry(entry, value);
        }
    }
    if (True(perms & Common::MemoryPermission::Execute) &&
        paddr + size <= DramMemoryMap::Base + mapped_backing_size) {
        sys_icache_invalidate(mapped_backing + (paddr - DramMemoryMap::Base), size);
        code_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    if (lost) {
        tlb_generation.fetch_add(1, std::memory_order_acq_rel);
        KickRunning();
    }
}

void Vm::Unmap(u64 vaddr, u64 size) {
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        for (u64 offset = 0; offset < size; offset += PageSize) {
            if (u64* entry = TableFor(vaddr + offset, false)) {
                StoreEntry(entry, 0);
            }
        }
    }
    tlb_generation.fetch_add(1, std::memory_order_acq_rel);
    KickRunning();
}

void Vm::Protect(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    bool lost = false;
    bool gained_execute = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        const u64 software = SoftwareFor(perms);
        for (u64 offset = 0; offset < size; offset += PageSize) {
            u64* entry = TableFor(vaddr + offset, false);
            if (entry == nullptr || (*entry & DescValid) != DescValid) {
                continue;
            }
            const u64 old = *entry;
            const u64 old_sw = old & (SwRead | SwWrite | SwExecute);
            const u64 new_sw = (old & (SwCachedNoWrite | SwCachedNoRead)) | software;
            if (old_sw & ~software) {
                lost = true;
            }
            if ((software & SwExecute) && !(old_sw & SwExecute)) {
                gained_execute = true;
            }
            StoreEntry(entry, Descriptor(old & DescAddressMask, new_sw));
        }
    }
    if (gained_execute) {
        SyncInstructionCache(vaddr, size);
    }
    if (lost) {
        tlb_generation.fetch_add(1, std::memory_order_acq_rel);
        KickRunning();
    }
}

void Vm::MarkCached(u64 vaddr, u64 size, bool cached, bool block_reads) {
    const u64 begin = Common::AlignDown(vaddr, PageSize);
    const u64 end = Common::AlignUp(vaddr + size, PageSize);
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        for (u64 page = begin; page < end; page += PageSize) {
            u64* entry = TableFor(page, false);
            if (entry == nullptr || (*entry & DescValid) != DescValid) {
                continue;
            }
            u64 software = *entry & SwMask;
            software &= ~(SwCachedNoWrite | SwCachedNoRead);
            if (cached) {
                software |= SwCachedNoWrite | (block_reads ? SwCachedNoRead : 0);
            }
            StoreEntry(entry, Descriptor(*entry & DescAddressMask, software));
        }
    }
    if (cached) {
        tlb_generation.fetch_add(1, std::memory_order_acq_rel);
        KickRunning();
    }
}

Vm::PageState Vm::Query(u64 vaddr) const {
    std::scoped_lock lk{mutex};
    PageState state{};
    if (!ready.load(std::memory_order_relaxed)) {
        return state;
    }
    u64* entry = const_cast<Vm*>(this)->TableFor(vaddr, false);
    if (entry == nullptr) {
        return state;
    }
    const u64 value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    if ((value & DescValid) != DescValid) {
        return state;
    }
    state.mapped = true;
    state.readable = (value & SwRead) && !(value & SwCachedNoRead);
    state.writable = state.readable && (value & SwWrite) && !(value & SwCachedNoWrite);
    state.executable = (value & SwExecute) != 0;
    state.cached = (value & (SwCachedNoWrite | SwCachedNoRead)) != 0;
    state.paddr = (value & DescAddressMask) | (vaddr & PageMask);
    return state;
}

void Vm::SyncInstructionCache(u64 vaddr, u64 size) {
    const u64 begin = Common::AlignDown(vaddr, PageSize);
    const u64 end = Common::AlignUp(vaddr + size, PageSize);
    for (u64 page = begin; page < end; page += PageSize) {
        const PageState state = Query(page);
        if (!state.mapped || state.paddr < DramMemoryMap::Base ||
            state.paddr - DramMemoryMap::Base + PageSize > mapped_backing_size) {
            continue;
        }
        sys_icache_invalidate(mapped_backing + (state.paddr - DramMemoryMap::Base), PageSize);
    }
    code_generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace Core::Hv
