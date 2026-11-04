// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = 65536;

    Result Observe(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            const u64 raw = instruction.words[1] | (u64{instruction.words[2] & 0x3ffff} << 32);
            const s64 relative = opcode == 0x945 ? static_cast<s32>(instruction.words[1])
                                                : static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            // CALL.REL.NOINC (bit 86) pushes no return address: the compiler uses it as a jump
            // (the lowering treats it as one), so it never pairs with a RET.
            const bool noinc_call = opcode == 0x944 && (instruction.words[2] & 0x400000) != 0;
            if (opcode == 0x944 && !noinc_call) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
            // The compiler ends every program with an unconditional branch to itself; with a jump
            // table (whose targets live in a constant buffer) that is the only reliable end.
            if (opcode == 0x947 && computed_branch && instruction.Bits(12, 4) == 7 &&
                target == s64{index} * 16)
                return Result::Complete;
            if (opcode == 0x947 && target == s64{index} * 16) NoteReachableEnd(instruction, index);
        }
        if (opcode == 0x950) NoteReachableEnd(instruction, index);
        if (opcode == 0x949 || opcode == 0x958) computed_branch = true;
        if (computed_branch) return Result::Continue;
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && index >= last_forward_target) {
            if (!last_call_target) return Result::Complete;
            main_exited = true;
        }
        if (opcode == 0x950 && instruction.Bits(12, 4) == 7 && main_exited &&
            index >= last_call_target && index >= last_forward_target)
            return Result::Complete;
        return Result::Continue;
    }

    static constexpr u32 NoReachableEnd = ~0U;

    // The first unconditional RET or branch-to-self that no earlier branch, BSSY or call targets
    // past (NoReachableEnd if none): nothing after it can run. The rules above can miss the end
    // (a NOINC call counts as a forward branch, so neither the main EXIT before the library
    // function nor the function's closing RET.REL.NODEC ends the capture); a program followed by
    // another one still completes at that one's EXIT, but one followed by data runs into it.
    [[nodiscard]] u32 ReachableEnd() const {
        return reachable_end;
    }

private:
    void NoteReachableEnd(const Instruction& instruction, u32 index) {
        // A jump table's targets are unknown; @P/@!P or a second condition (87..90) falls through.
        if (reachable_end != NoReachableEnd || computed_branch || instruction.Bits(12, 4) != 7 ||
            instruction.Bits(87, 4) != 7)
            return;
        if (index >= last_forward_target && index >= last_call_target) reachable_end = index;
    }

    u32 last_call_target{};
    u32 last_forward_target{};
    u32 reachable_end{NoReachableEnd};
    bool main_exited{};
    bool computed_branch{};
};
} // namespace Shader::Ampere
