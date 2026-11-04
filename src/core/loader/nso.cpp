// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}

// Module offset of an exported symbol, read from the image's MOD0 dynamic section. The symbol
// table directly precedes the string table in SDK modules.
std::optional<size_t> FindExportedSymbol(std::span<const u8> image, std::string_view symbol) {
    const auto read = [image]<typename T>(size_t offset, T& value) {
        if (offset + sizeof(T) > image.size()) {
            return false;
        }
        std::memcpy(&value, image.data() + offset, sizeof(T));
        return true;
    };
    u32 mod_offset = 0;
    MODHeader mod{};
    if (!read(4, mod_offset) || !read(mod_offset, mod) ||
        mod.magic != Common::MakeMagic('M', 'O', 'D', '0')) {
        return std::nullopt;
    }
    u64 strtab = 0, symtab = 0;
    for (size_t entry = mod_offset + static_cast<s32>(mod.dynamic_offset);; entry += 16) {
        s64 tag = 0;
        u64 value = 0;
        if (!read(entry, tag) || !read(entry + 8, value) || tag == 0) {
            break;
        }
        if (tag == 5 && strtab == 0) { // DT_STRTAB
            strtab = value;
        } else if (tag == 6 && symtab == 0) { // DT_SYMTAB
            symtab = value;
        }
    }
    if (strtab == 0 || symtab == 0 || strtab >= image.size()) {
        return std::nullopt;
    }
    for (u64 entry = symtab + 24; entry + 24 <= strtab; entry += 24) {
        u32 name = 0;
        u16 section = 0;
        u64 value = 0;
        read(entry, name);
        read(entry + 6, section);
        read(entry + 8, value);
        if (value == 0 || section == 0 || strtab + name + symbol.size() >= image.size()) {
            continue;
        }
        const char* const text = reinterpret_cast<const char*>(image.data() + strtab + name);
        if (std::string_view{text, symbol.size()} == symbol && text[symbol.size()] == '\0') {
            return value;
        }
    }
    return std::nullopt;
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // Mario Kart World picks how many vblanks each frame lasts (its present interval) from a mode
    // in its frame loop: 0 = from the frame time, 1 = 60 fps, 2 = 30 fps, and advances its clock by
    // that many vblanks a frame. Forcing 2 gives a steady 30 fps at normal game speed. The mode's
    // load is found by its code (same in 1.0.0 at 0x3A9EC8 and 1.8.0 at 0x69CC54):
    //   cmp w9, w1; b.ne; ldr w8, [x19, #0x210]; cbnz w8; ... mov w8, #0x42700000 (60.0f)
    //
    // Dynamic mode (mkw_dynamic_fps) forces mode 0. The game then sets each frame's interval (1 or
    // 2 vblanks) from how late its frames run, but it times only its own CPU work ([fw+0x128],
    // calc), which is fast under emulation where the GPU thread is the slow part: it stayed at 1
    // and the game ran in slow motion. The same loop also stores the whole frame's time
    // ([fw+0xe8], calc + draw + present), so the interval code is pointed at that one.
    const bool mkw_dynamic = Settings::values.mkw_dynamic_fps.GetValue();
    if ((mkw_dynamic || Settings::values.mkw_30fps.GetValue()) && name == "main" &&
        process.GetProgramId() == 0x0400C3F00006E000ULL) {
        constexpr u32 ModeLoadInsn = 0xB9421268; // ldr w8, [x19, #0x210]
        const u32 Force30Insn = mkw_dynamic ? 0x52800008U  // mov w8, #0
                                            : 0x52800048U; // mov w8, #2
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        const size_t count = text.size / sizeof(u32);
        const auto word = [code](size_t index) {
            u32 value;
            std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
            return value;
        };
        size_t found = 0;
        size_t matches = 0;
        for (size_t i = 2; i + 8 < count; ++i) {
            if (word(i) != ModeLoadInsn || word(i - 2) != 0x6B01013F || // cmp w9, w1
                (word(i - 1) & 0xFF00001F) != 0x54000001 ||             // b.ne
                (word(i + 1) & 0xFF00001F) != 0x35000008) {             // cbnz w8
                continue;
            }
            for (size_t j = i + 2; j < i + 8; ++j) {
                if (word(j) == 0x52A84E08) { // mov w8, #0x42700000
                    found = i;
                    ++matches;
                    break;
                }
            }
        }
        // Dynamic: the frame time load after it, ldr xN, [x19, #0x128] -> [x19, #0xe8].
        size_t frame_time = 0;
        if (matches == 1 && mkw_dynamic) {
            for (size_t j = found + 1; j < found + 32 && j < count; ++j) {
                if ((word(j) & 0xFFFFFFE0) == 0xF9409660) {
                    frame_time = j;
                    break;
                }
            }
        }
        if (matches == 1 && mkw_dynamic && frame_time == 0) {
            LOG_WARNING(Loader, "Mario Kart World: dynamic FPS not applied (frame time not found)");
        } else if (matches == 1) {
            std::memcpy(code + found * sizeof(u32), &Force30Insn, sizeof(Force30Insn));
            if (mkw_dynamic) {
                const u32 whole_frame = 0xF9407660 | (word(frame_time) & 0x1F);
                std::memcpy(code + frame_time * sizeof(u32), &whole_frame, sizeof(whole_frame));
            }
            LOG_INFO(Loader, "Mario Kart World: {} on (main+{:#x})",
                     mkw_dynamic ? "dynamic FPS" : "30 FPS mode",
                     text.location + found * sizeof(u32));
        } else {
            LOG_WARNING(Loader, "Mario Kart World: 30 FPS mode not applied ({} matches)", matches);
        }
    }

    // Donkey Kong Bananza has no vblank-mode selector like Mario Kart World. Its frame rate
    // controller calls nn::vi::SetLayerExtendedSwapInterval(layer, 1, fps) from two sites; forcing
    // fps = 30 there locks the game to 30. Sites are found by their code (same in base and 458752):
    //   A: ldr x0,[x19,#0x180]; mov w1,#1; ldr w2,[x19,#0xE0]; bl   (patch the ldr w2)
    //   B: ldr x0,[x0,#0x180];  mov w1,#1; mov w2,w19;          bl   (patch the mov w2)
    if (Settings::values.dk_30fps.GetValue() && name == "main" &&
        process.GetProgramId() == 0x040074A01BF12000ULL) {
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        const size_t count = text.size / sizeof(u32);
        const auto word = [code](size_t index) {
            u32 value;
            std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
            return value;
        };
        size_t site_a = 0, site_b = 0, matches_a = 0, matches_b = 0;
        for (size_t i = 0; i + 3 < count; ++i) {
            if ((word(i + 3) & 0xFC000000) != 0x94000000 || word(i + 1) != 0x52800021) {
                continue; // bl, mov w1,#1
            }
            if (word(i) == 0xF940C260 && word(i + 2) == 0xB940E262) {
                site_a = i + 2;
                ++matches_a;
            } else if (word(i) == 0xF940C000 && word(i + 2) == 0x2A1303E2) {
                site_b = i + 2;
                ++matches_b;
            }
        }
        // The controller's own dynamic divisor (vblanks per frame, 1..3) drives the present interval.
        // Pin it: mov w8,#60; ldr w9,[x20,#0x24]; str w21,[x20,#0x40]; sdiv w8,w8,w21 -> w21=2, w8=30.
        size_t site_div = 0, matches_div = 0;
        for (size_t i = 0; i + 4 < count; ++i) {
            if (word(i) == 0x52800788 && word(i + 1) == 0xB9402689 && word(i + 2) == 0xB9004295 &&
                word(i + 3) == 0x1AD50D08 && word(i + 4) == 0x2A0803E1) {
                site_div = i;
                ++matches_div;
            }
        }
        if (matches_a == 1 && matches_b == 1 && matches_div == 1) {
            constexpr u32 Mov30 = 0x528003C2;   // mov w2, #30
            constexpr u32 MovDiv2 = 0x52800055; // mov w21, #2
            constexpr u32 MovFps30 = 0x528003C8; // mov w8, #30
            std::memcpy(code + site_a * sizeof(u32), &Mov30, sizeof(Mov30));
            std::memcpy(code + site_b * sizeof(u32), &Mov30, sizeof(Mov30));
            std::memcpy(code + site_div * sizeof(u32), &MovDiv2, sizeof(MovDiv2));
            std::memcpy(code + (site_div + 3) * sizeof(u32), &MovFps30, sizeof(MovFps30));
            LOG_INFO(Loader,
                     "Donkey Kong Bananza: 30 FPS lock on (main+{:#x}, main+{:#x}, main+{:#x})",
                     text.location + site_a * sizeof(u32), text.location + site_b * sizeof(u32),
                     text.location + site_div * sizeof(u32));
        } else {
            LOG_WARNING(Loader, "Donkey Kong Bananza: 30 FPS lock not applied ({}/{}/{} matches)",
                        matches_a, matches_b, matches_div);
        }
    }

    // Metroid Prime 4 Beyond ships one Switch 1 program for both editions: nnMain stores
    // nn::pl::IsRunningOnOunce() in a global that selects the Switch 2 Edition. The SDK's version
    // only returns false (mov w0, wzr; ret); a Switch 2 with the edition's unlock content reports
    // true. EDEN_NX2_OUNCE=0/1 overrides the setting.
    if (load_into_process && name == "sdk" && process.GetProgramId() == 0x010019A01E2F2000ULL) {
        bool ounce = Settings::values.mp4_switch2_edition.GetValue();
        if (const char* env = std::getenv("EDEN_NX2_OUNCE"); env && *env) {
            ounce = *env != '0';
        }
        const std::span<u8> image(codeset.memory.data() + module_start,
                                  codeset.memory.size() - module_start);
        const auto symbol = FindExportedSymbol(image, "_ZN2nn2pl16IsRunningOnOunceEv");
        if (ounce && symbol && *symbol + 8 <= nso_header.segments[0].size) {
            constexpr std::array<u32, 2> ReturnTrue{0x52800020, 0xD65F03C0}; // mov w0, #1; ret
            std::memcpy(image.data() + *symbol, ReturnTrue.data(), sizeof(ReturnTrue));
            LOG_INFO(Loader, "Metroid Prime 4: Switch 2 Edition mode on (sdk+{:#x})", *symbol);
        } else if (ounce) {
            LOG_WARNING(Loader, "Metroid Prime 4: nn::pl::IsRunningOnOunce not found in sdk");
        } else {
            LOG_INFO(Loader, "Metroid Prime 4: Switch 1 version (Switch 2 Edition mode off)");
        }
    }

    // Metroid Prime 4's rumble mixer keeps its vibration sources in sync with displayed frames: a
    // source counts as active once the GPU frame queue has completed the frame it started on
    // (completed [q+0x350] >= stamp taken from submitted [q+0x348]). Each frame the mixer re-pushes
    // the active sources and pops one, without checking that any was pushed. The console's GPU
    // finishes a frame within one CPU frame, so the always-present idle source is active by the
    // second mixer pass; when Eden's GPU runs a frame behind at boot it is not, the pop underflows
    // the stack (its count is overwritten) and the next pass crashes (main+0x115a34 in 2.0.0, about
    // one boot in seven). The idle source is the "no vibration" base, so make it always active.
    // Same code in 1.0.0 (main+0xdd22b4) and 2.0.0 (main+0xdc5c54). EDEN_NX2_MP4_VIBFIX=0 skips it.
    const char* vibfix_env = std::getenv("EDEN_NX2_MP4_VIBFIX");
    if (load_into_process && name == "main" && process.GetProgramId() == 0x010019A01E2F2000ULL &&
        !(vibfix_env && *vibfix_env == '0')) {
        constexpr std::array<u32, 13> IdleIsActive{
            0xF9401C08, // ldr x8, [x0, #0x38]       stamp
            0xB40000A8, // cbz x8, inactive
            0xF944BC29, // ldr x9, [x1, #0x978]      frame queue
            0xF941A929, // ldr x9, [x9, #0x350]      completed
            0xEB08013F, // cmp x9, x8
            0x54000062, // b.hs active
            0x2A1F03E0, // mov w0, wzr
            0xD65F03C0, // ret
            0x52800020, // mov w0, #1
            0xD65F03C0, // ret
            0xF944BC28, // Start: ldr x8, [x1, #0x978]
            0xF941A508, //        ldr x8, [x8, #0x348]  submitted
            0xF9001C08, //        str x8, [x0, #0x38]
        };
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        size_t found = 0;
        size_t matches = 0;
        for (size_t offset = 0; offset + sizeof(IdleIsActive) <= text.size; offset += 4) {
            if (std::memcmp(code + offset, IdleIsActive.data(), sizeof(IdleIsActive)) == 0) {
                found = offset;
                ++matches;
            }
        }
        if (matches == 1) {
            constexpr std::array<u32, 2> ReturnTrue{0x52800020, 0xD65F03C0}; // mov w0, #1; ret
            std::memcpy(code + found, ReturnTrue.data(), sizeof(ReturnTrue));
            LOG_INFO(Loader, "Metroid Prime 4: idle vibration source always active (main+{:#x})",
                     text.location + found);
        } else {
            LOG_WARNING(Loader, "Metroid Prime 4: idle vibration source not patched ({} matches)",
                        matches);
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
