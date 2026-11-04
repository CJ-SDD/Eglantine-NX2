// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <opus.h>
#include <opus_multistream.h>

#include "common/common_funcs.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/memory.h"

namespace Service::Audio {

// Switch 2 hardware audio codec (nn::codec::hwacod), reached through hwopus command 100. The
// Switch 2 SDK decodes Opus on the ADSP instead of through IHardwareOpusDecoder:
// - hwopus 100 returns an IClientSession (domain object).
// - IClientSession 0(X: parameter) and 1(u32 count) return work buffer sizes; 10 takes the shared
//   memory layout, the transfer memory and the address of a work area in the process, and returns
//   a u32 client id, a readable event and an IHardwareAudioCodecManagerSession.
// - The manager session creates (0) and destroys (1) decoder instances and takes batches of
//   16-byte tasks (10). Packets sit in I/O buffers of the work area; the server writes the decoded
//   PCM next to them and appends one 0x40-byte entry per task to a result ring in the same area,
//   then signals the event (HardwareAudioCodecManager::UpdateProcessResult reads the ring).
// Everything is decoded on the host with libopus. Names keep the nx2: prefix, so unknown commands
// stay auto-stubbed and logged.

// Instance parameter as sent by the SDK (0x120 bytes; the variant index is remapped to kind).
struct Nx2HwacodParameter {
    enum class Kind : u64 { Stub = 0, Opus = 1, OpusMultiStream = 2, None = 3 };

    Kind kind;
    s32 sample_rate;
    s32 channel_count;
    s64 frame_duration_ns;
    s32 total_stream_count;
    s32 stereo_stream_count;
    std::array<u8, 0x100> mappings;
};
static_assert(sizeof(Nx2HwacodParameter) == 0x120);

// WorkBufferSizeCalculator::GetSharedMemoryParameter, the first 0x60 bytes of IClientSession 10.
struct Nx2HwacodSharedMemoryParameter {
    u32 version;
    INSERT_PADDING_BYTES(0x20);
    u32 io_buffer_offset;   // from the start of the work area
    INSERT_PADDING_BYTES(4);
    u32 result_ring_offset; // from the start of the work area
    INSERT_PADDING_BYTES(0xc);
    u32 io_input_size;
    u32 io_output_size;
    u32 io_buffer_count;
    INSERT_PADDING_BYTES(0x18);
};
static_assert(sizeof(Nx2HwacodSharedMemoryParameter) == 0x60);

struct Nx2HwacodOpenArgs {
    Nx2HwacodSharedMemoryParameter shared_memory;
    u64 transfer_memory_size;
    u64 work_area_address;
    u64 work_area_size;
    u64 properties;
};
static_assert(sizeof(Nx2HwacodOpenArgs) == 0x80);

struct Nx2HwacodTask {
    u16 client_id;
    u16 instance_id;
    u16 io_buffer_index;
    u16 sequence;
    u32 input_size;
    u8 reset;
    u8 last;
    INSERT_PADDING_BYTES(2);
};
static_assert(sizeof(Nx2HwacodTask) == 0x10);

struct Nx2HwacodResult {
    u16 client_id;
    u16 instance_id;
    u16 io_buffer_index;
    u16 sequence;
    s64 processing_time_ns;
    u32 output_size;
    u32 result;
    INSERT_PADDING_BYTES(0x28);
};
static_assert(sizeof(Nx2HwacodResult) == 0x40);

class Nx2HwacodDecoder {
public:
    explicit Nx2HwacodDecoder(const Nx2HwacodParameter& param_) : param{param_} {
        int error = OPUS_OK;
        if (param.kind == Nx2HwacodParameter::Kind::Opus) {
            decoder = opus_decoder_create(param.sample_rate, param.channel_count, &error);
        } else if (param.kind == Nx2HwacodParameter::Kind::OpusMultiStream) {
            const int coupled = param.stereo_stream_count;
            multistream = opus_multistream_decoder_create(
                param.sample_rate, param.channel_count, param.total_stream_count, coupled,
                param.mappings.data(), &error);
        }
        if (error != OPUS_OK) {
            LOG_ERROR(Service_Audio, "nx2:hwacod: decoder creation failed ({}), rate {} channels {}",
                      error, param.sample_rate, param.channel_count);
        }
    }

    ~Nx2HwacodDecoder() {
        if (decoder) {
            opus_decoder_destroy(decoder);
        }
        if (multistream) {
            opus_multistream_decoder_destroy(multistream);
        }
    }

    Nx2HwacodDecoder(const Nx2HwacodDecoder&) = delete;
    Nx2HwacodDecoder& operator=(const Nx2HwacodDecoder&) = delete;

    // Input is an OpusPacketHeader (big-endian size and final range) followed by the packet.
    // Returns the number of output bytes (interleaved s16). A bad packet decodes to one frame of
    // silence, so a stream never stalls on it.
    u32 Decode(std::span<const u8> input, std::span<s16> output, bool reset) {
        const int channels = (std::max)(param.channel_count, 1);
        const int max_samples = static_cast<int>(output.size() / channels);
        if (reset) {
            if (decoder) {
                opus_decoder_ctl(decoder, OPUS_RESET_STATE);
            }
            if (multistream) {
                opus_multistream_decoder_ctl(multistream, OPUS_RESET_STATE);
            }
        }
        int samples = -1;
        if (input.size() >= 8 && (decoder || multistream)) {
            u32 packet_size{};
            std::memcpy(&packet_size, input.data(), sizeof(packet_size));
            packet_size = Common::swap32(packet_size);
            const auto packet =
                input.subspan(8, (std::min)(static_cast<size_t>(packet_size), input.size() - 8));
            const auto size = static_cast<opus_int32>(packet.size());
            samples = decoder ? opus_decode(decoder, packet.data(), size, output.data(),
                                            max_samples, 0)
                              : opus_multistream_decode(multistream, packet.data(), size,
                                                        output.data(), max_samples, 0);
        }
        if (samples < 0) {
            if (!warned_bad_packet) {
                warned_bad_packet = true;
                LOG_WARNING(Service_Audio, "nx2:hwacod: packet of {} bytes not decoded ({}), output silence",
                            input.size(), samples);
            }
            samples = (std::min)(FrameSamples(), max_samples);
            std::fill_n(output.begin(), samples * channels, s16{0});
        }
        return static_cast<u32>(samples * channels * sizeof(s16));
    }

private:
    int FrameSamples() const {
        const s64 samples = (param.frame_duration_ns * param.sample_rate + 999'999'999) / 1'000'000'000;
        return static_cast<int>(std::clamp<s64>(samples, 0, 5760));
    }

    Nx2HwacodParameter param;
    OpusDecoder* decoder{};
    OpusMSDecoder* multistream{};
    bool warned_bad_packet{};
};

// State shared by the manager session and its result timer.
struct Nx2HwacodState {
    std::mutex mutex;
    Core::Memory::Memory* memory{};
    Kernel::KernelCore* kernel{};
    Kernel::KEvent* event{};
    VAddr io_base{};
    VAddr ring_base{};
    u32 io_input_size{};
    u32 io_output_size{};
    u32 ring_count{};
    u32 ring_write{};
    std::vector<Nx2HwacodResult> pending;

    // Appends the finished tasks to the result ring: header +4 is the server's write index,
    // entries start at +0x80.
    void Publish() {
        std::scoped_lock lk{mutex};
        if (pending.empty()) {
            return;
        }
        for (const auto& result : pending) {
            memory->WriteBlock(ring_base + 0x80 + ring_write * sizeof(Nx2HwacodResult), &result,
                               sizeof(result));
            ring_write = (ring_write + 1) % ring_count;
        }
        pending.clear();
        memory->Write32(ring_base + 4, ring_write);
        event->Signal(*kernel);
    }
};

class INx2HardwareAudioCodecManagerSession final
    : public ServiceFramework<INx2HardwareAudioCodecManagerSession> {
public:
    explicit INx2HardwareAudioCodecManagerSession(Core::System& system_,
                                                  const Nx2HwacodOpenArgs& args)
        : ServiceFramework{system_, "nx2:hwacod:manager"}, m_context{system_, "nx2:hwacod"},
          m_state{std::make_shared<Nx2HwacodState>()} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2HardwareAudioCodecManagerSession::InitializeInstance, "InitializeInstance (guessed)"},
            {1, &INx2HardwareAudioCodecManagerSession::FinalizeInstance, "FinalizeInstance (guessed)"},
            {10, &INx2HardwareAudioCodecManagerSession::SubmitTasks, "SubmitTasks (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);

        const auto& shared = args.shared_memory;
        auto& state = *m_state;
        state.memory = &system.ApplicationMemory();
        state.kernel = &system.Kernel();
        state.event = m_context.CreateEvent("nx2:hwacod:Result");
        state.io_base = args.work_area_address + shared.io_buffer_offset;
        state.ring_base = args.work_area_address + shared.result_ring_offset;
        state.io_input_size = shared.io_input_size;
        state.io_output_size = shared.io_output_size;
        state.ring_count = shared.io_buffer_count + 1;
        LOG_INFO(Service_Audio,
                 "nx2:hwacod: work area {:#x}+{:#x}, {} I/O buffers of {:#x}+{:#x} at +{:#x}, "
                 "result ring at +{:#x}",
                 args.work_area_address, args.work_area_size, shared.io_buffer_count,
                 shared.io_input_size, shared.io_output_size, shared.io_buffer_offset,
                 shared.result_ring_offset);

        m_timer = Core::Timing::CreateEvent(
            "nx2:hwacod results",
            [weak = std::weak_ptr{m_state}](s64, std::chrono::nanoseconds)
                -> std::optional<std::chrono::nanoseconds> {
                if (auto state = weak.lock()) {
                    state->Publish();
                }
                return std::nullopt;
            });
    }

    ~INx2HardwareAudioCodecManagerSession() override {
        system.CoreTiming().UnscheduleEvent(m_timer);
        m_context.CloseEvent(m_state->event);
    }

    Kernel::KReadableEvent& GetResultEvent() {
        return m_state->event->GetReadableEvent();
    }

private:
    // Results are published a little later rather than inside the request: Execute marks the
    // I/O buffers as processing only after this call returns, and a result seen before that
    // breaks the SDK's buffer lists.
    static constexpr std::chrono::microseconds ResultLatency{500};

    void InitializeInstance(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 instance_id = rp.Pop<u32>();
        Nx2HwacodParameter param{};
        const auto buffer = ctx.ReadBuffer();
        std::memcpy(&param, buffer.data(), (std::min)(buffer.size(), sizeof(param)));
        LOG_INFO(Service_Audio,
                 "nx2:hwacod: instance {} kind {} rate {} channels {} frame {} ns streams {}/{}",
                 instance_id, static_cast<u64>(param.kind), param.sample_rate, param.channel_count,
                 param.frame_duration_ns, param.total_stream_count, param.stereo_stream_count);
        {
            std::scoped_lock lk{m_state->mutex};
            m_decoders[instance_id] = std::make_unique<Nx2HwacodDecoder>(param);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void FinalizeInstance(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 instance_id = rp.Pop<u32>();
        {
            std::scoped_lock lk{m_state->mutex};
            m_decoders.erase(instance_id);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SubmitTasks(HLERequestContext& ctx) {
        const auto buffer = ctx.ReadBuffer();
        std::vector<Nx2HwacodTask> tasks(buffer.size() / sizeof(Nx2HwacodTask));
        std::memcpy(tasks.data(), buffer.data(), tasks.size() * sizeof(Nx2HwacodTask));

        auto& state = *m_state;
        {
            std::scoped_lock lk{state.mutex};
            const u32 stride = state.io_input_size + state.io_output_size;
            for (const auto& task : tasks) {
                const VAddr input = state.io_base + VAddr{task.io_buffer_index} * stride;
                const VAddr output = input + state.io_input_size;
                m_input.resize((std::min)(task.input_size, state.io_input_size));
                state.memory->ReadBlock(input, m_input.data(), m_input.size());
                m_output.assign(state.io_output_size / sizeof(s16), 0);

                const auto start = std::chrono::steady_clock::now();
                u32 output_size = 0;
                if (const auto it = m_decoders.find(task.instance_id); it != m_decoders.end()) {
                    output_size = it->second->Decode(m_input, m_output, task.reset != 0);
                } else {
                    LOG_WARNING(Service_Audio, "nx2:hwacod: task for unknown instance {}",
                                task.instance_id);
                }
                state.memory->WriteBlock(output, m_output.data(), output_size);
                const auto elapsed = std::chrono::steady_clock::now() - start;

                state.pending.push_back({
                    .client_id = task.client_id,
                    .instance_id = task.instance_id,
                    .io_buffer_index = task.io_buffer_index,
                    .sequence = task.sequence,
                    .processing_time_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
                    .output_size = output_size,
                    .result = 0,
                });
            }
        }
        if (!tasks.empty()) {
            system.CoreTiming().ScheduleEvent(ResultLatency, m_timer);
        }

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2HwacodState> m_state;
    std::shared_ptr<Core::Timing::EventType> m_timer;
    std::map<u32, std::unique_ptr<Nx2HwacodDecoder>> m_decoders;
    std::vector<u8> m_input;
    std::vector<s16> m_output;
};

class INx2HardwareAudioCodecClientSession final
    : public ServiceFramework<INx2HardwareAudioCodecClientSession> {
public:
    explicit INx2HardwareAudioCodecClientSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:hwacod"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2HardwareAudioCodecClientSession::GetInstanceWorkBufferSize, "GetInstanceWorkBufferSize (guessed)"},
            {1, &INx2HardwareAudioCodecClientSession::GetManagerWorkBufferSize, "GetManagerWorkBufferSize (guessed)"},
            {10, &INx2HardwareAudioCodecClientSession::OpenManagerSession, "OpenManagerSession (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    // Room the server would keep per instance / for the manager inside the work area. Nothing is
    // stored there on the host; the SDK only adds these to its layout (and CanInstantiate compares
    // the instance size with the one the manager was sized for, so it must not depend on the
    // parameter).
    static constexpr u32 InstanceWorkBufferSize = 0x100;
    static constexpr u32 ManagerWorkBufferSize = 0x1000;

    void GetInstanceWorkBufferSize(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(InstanceWorkBufferSize);
    }

    void GetManagerWorkBufferSize(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(ManagerWorkBufferSize);
    }

    void OpenManagerSession(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto args = rp.PopRaw<Nx2HwacodOpenArgs>();
        auto session = std::make_shared<INx2HardwareAudioCodecManagerSession>(system, args);

        // The client id goes into the low 16 bits of every task header.
        IPC::ResponseBuilder rb{ctx, 3, 1, 1};
        rb.Push(ResultSuccess);
        rb.Push<u32>(ClientId);
        rb.PushCopyObjects(ctx, session->GetResultEvent());
        rb.PushIpcInterface(ctx, std::move(session));
    }

    static constexpr u32 ClientId = 1;
};

} // namespace Service::Audio
