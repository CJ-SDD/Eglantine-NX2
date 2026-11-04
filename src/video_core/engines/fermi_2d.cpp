// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/sw_blitter/blitter.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

namespace Tegra::Engines {

using namespace Texture;

namespace {

// A 2D engine surface is a 2D slice of a block-linear surface `depth` slices deep: the engine reads
// or writes slice `layer`. Slices are laid out as in a 3D texture: (layer >> block_depth) whole
// layers of blocks, then (layer & mask) GOB rows of block_height GOBs inside each block. Rebase the
// surface onto that slice (the address offset nouveau and NVN use for a 3D slice, with layer 0),
// so the texture cache finds it as that slice of the 3D image (ImageBase::slice_offsets) and the
// software blitter reads and writes that one slice. block_depth stays: the slice keeps the
// 3D block stride. Pitch surfaces have no slices.
void SelectSurfaceSlice(Fermi2D::Surface& surface) {
    if (surface.depth == 1 && surface.layer == 0) {
        return;
    }
    const u32 slice = surface.layer;
    surface.depth = 1;
    surface.layer = 0;
    if (surface.linear == Fermi2D::MemoryLayout::Pitch || slice == 0) {
        return;
    }
    const u64 bytes_per_pixel = BytesPerBlock(PixelFormatFromRenderTargetFormat(surface.format));
    const u32 block_height = surface.block_height;
    const u32 block_depth = surface.block_depth;
    const u64 gobs_x = (u64{surface.width} * bytes_per_pixel + GOB_SIZE_X - 1) >> GOB_SIZE_X_SHIFT;
    const u32 block_lines_shift = GOB_SIZE_Y_SHIFT + block_height;
    const u64 blocks_y = (u64{surface.height} + (1ULL << block_lines_shift) - 1) >> block_lines_shift;
    const u64 block_layer_size = (gobs_x * blocks_y) << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 z_mask = (1U << block_depth) - 1;
    const u64 offset = u64{slice >> block_depth} * block_layer_size +
                       (u64{slice & z_mask} << (GOB_SIZE_SHIFT + block_height));
    const GPUVAddr address = surface.Address() + offset;
    surface.addr_upper = static_cast<u32>(address >> 32);
    surface.addr_lower = static_cast<u32>(address);
}

} // Anonymous namespace

Fermi2D::Fermi2D(MemoryManager& memory_manager_) : memory_manager{memory_manager_} {
    sw_blitter = std::make_unique<Blitter::SoftwareBlitEngine>(memory_manager);
    // Nvidia's OpenGL driver seems to assume these values
    regs.src.depth = 1;
    regs.dst.depth = 1;

    execution_mask.reset();
    execution_mask[FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1] = true;
}

Fermi2D::~Fermi2D() = default;

void Fermi2D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void Fermi2D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid Fermi2D register, increase the size of the Regs structure");
    regs.reg_array[method] = method_argument;

    if (method == FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1) {
        Blit();
    }
}

void Fermi2D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(system, method, base_start[i], methods_pending - i <= 1);
    }
}

void Fermi2D::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void Fermi2D::Blit() {
    if (static const u64 trace_destination = [] {
            const char* value = std::getenv("EDEN_NX2_TRACE_DST");
            return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
        }();
        trace_destination != 0 && regs.dst.Address() >= trace_destination &&
        regs.dst.Address() < trace_destination + (32ULL << 20)) {
        static u32 traced_destination{};
        if (traced_destination++ < 40) {
            LOG_INFO(HW_GPU, "nx2 2D blit to traced dst: {:#x} {}x{} -> {:#x} {}x{}",
                     regs.src.Address(), regs.src.width, regs.src.height, regs.dst.Address(),
                     regs.dst.width, regs.dst.height);
        }
    }
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        static u32 traced{};
        if (traced++ < 64) {
            LOG_INFO(HW_GPU, "nx2 2D blit {:#x} {}x{} -> {:#x} {}x{}", regs.src.Address(),
                     regs.src.width, regs.src.height, regs.dst.Address(), regs.dst.width,
                     regs.dst.height);
        }
    }
    LOG_DEBUG(HW_GPU, "called. source address={:#x}, destination address={:#x}", regs.src.Address(), regs.dst.Address());

    UNIMPLEMENTED_IF_MSG(regs.operation != Operation::SrcCopy, "Operation is not copy");
    UNIMPLEMENTED_IF_MSG(regs.clip_enable != 0, "Clipped blit enabled");

    const auto& args = regs.pixels_from_memory;
    constexpr s64 null_derivative = 1ULL << 32;
    Surface src = regs.src;
    Surface dst = regs.dst;
    // Surfaces with depth != 1 (slices of 3D textures, UE5 on Switch 2): copy slice `layer`.
    SelectSurfaceSlice(src);
    SelectSurfaceSlice(dst);
    if (regs.src.depth != 1 || regs.src.layer != 0 || regs.dst.depth != 1 || regs.dst.layer != 0) {
        static u32 logged_slices{};
        if (logged_slices++ < 16) {
            LOG_INFO(HW_GPU,
                     "2D blit slice: src {:#x} {}x{}x{} layer {} block_depth {} -> {:#x}{}, dst "
                     "{:#x} {}x{}x{} layer {} block_depth {} -> {:#x}{}",
                     regs.src.Address(), regs.src.width, regs.src.height, regs.src.depth,
                     regs.src.layer, regs.src.block_depth.Value(), src.Address(),
                     regs.src.layer >= regs.src.depth ? " (layer past depth)" : "",
                     regs.dst.Address(), regs.dst.width, regs.dst.height, regs.dst.depth,
                     regs.dst.layer, regs.dst.block_depth.Value(), dst.Address(),
                     regs.dst.layer >= regs.dst.depth ? " (layer past depth)" : "");
        }
    }
    const auto bytes_per_pixel = BytesPerBlock(PixelFormatFromRenderTargetFormat(src.format));
    const bool delegate_to_gpu = src.width > 512 && src.height > 512 && bytes_per_pixel <= 8 &&
                                 src.format != regs.dst.format;

    auto srcX = args.src_x0;
    auto srcY = args.src_y0;
    if (args.sample_mode.origin == Origin::Corner) {
        srcX -= (args.du_dx >> 33) << 32;
        srcY -= (args.dv_dy >> 33) << 32;
    }

    Config config{
        .operation = regs.operation,
        .filter = args.sample_mode.filter,
        .must_accelerate =
            args.du_dx != null_derivative || args.dv_dy != null_derivative || delegate_to_gpu,
        .dst_x0 = args.dst_x0,
        .dst_y0 = args.dst_y0,
        .dst_x1 = args.dst_x0 + args.dst_width,
        .dst_y1 = args.dst_y0 + args.dst_height,
        .src_x0 = static_cast<s32>(srcX >> 32),
        .src_y0 = static_cast<s32>(srcY >> 32),
        .src_x1 = static_cast<s32>((srcX + args.du_dx * args.dst_width) >> 32),
        .src_y1 = static_cast<s32>((srcY + args.dv_dy * args.dst_height) >> 32),
    };

    const auto need_align_to_pitch =
        src.linear == Tegra::Engines::Fermi2D::MemoryLayout::Pitch &&
        static_cast<s32>(src.width) == config.src_x1 &&
        config.src_x1 > static_cast<s32>(src.pitch / bytes_per_pixel) && config.src_x0 > 0;
    if (need_align_to_pitch) {
        auto address = src.Address() + config.src_x0 * bytes_per_pixel;
        src.addr_upper = static_cast<u32>(address >> 32);
        src.addr_lower = static_cast<u32>(address);
        src.width -= config.src_x0;
        config.src_x1 -= config.src_x0;
        config.src_x0 = 0;
    }

    memory_manager.FlushCaching();
    if (!rasterizer->AccelerateSurfaceCopy(src, dst, config)) {
        sw_blitter->Blit(src, dst, config);
    }
}

} // namespace Tegra::Engines
