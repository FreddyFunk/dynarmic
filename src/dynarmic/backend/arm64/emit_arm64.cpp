/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64.h"

#include <fmt/ostream.h>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

template<>
void EmitIR<IR::Opcode::Void>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*) {}

template<>
void EmitIR<IR::Opcode::Identity>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.DefineAsExisting(inst, args[0]);
}

template<>
void EmitIR<IR::Opcode::Breakpoint>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.BRK(0);
}

template<>
void EmitIR<IR::Opcode::CallHostFunction>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    ASSERT_FALSE("Unimplemented");
}

template<>
void EmitIR<IR::Opcode::PushRSB>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*) {
    // TODO
}

template<>
void EmitIR<IR::Opcode::GetCarryFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetOverflowFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetGEFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetNZCVFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetNZFromOp>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (ctx.reg_alloc.IsValueLive(inst)) {
        return;
    }

    switch (args[0].GetType()) {
    case IR::Type::U32: {
        auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Wvalue, flags);

        code.TST(*Wvalue, *Wvalue);
        break;
    }
    case IR::Type::U64: {
        auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Xvalue, flags);

        code.TST(*Xvalue, *Xvalue);
        break;
    }
    default:
        ASSERT_FALSE("Invalid type for GetNZFromOp");
        break;
    }
}

template<>
void EmitIR<IR::Opcode::GetUpperFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetLowerFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.IsValueLive(inst));
}

template<>
void EmitIR<IR::Opcode::GetCFlagFromNZCV>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Wc = ctx.reg_alloc.WriteW(inst);
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wc, Wnzcv);

    code.AND(Wc, Wnzcv, 1 << 29);
}

template<>
void EmitIR<IR::Opcode::NZCVFromPackedFlags>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.DefineAsExisting(inst, args[0]);
}

static void EmitAddCycles(oaknut::CodeGenerator& code, EmitContext&, size_t cycles_to_add) {
    code.LDR(Xscratch0, SP, offsetof(StackLayout, cycles_remaining));
    if (oaknut::AddSubImm::is_valid(cycles_to_add)) {
        code.SUBS(Xscratch0, Xscratch0, cycles_to_add);
    } else {
        code.MOV(Xscratch1, cycles_to_add);
        code.SUBS(Xscratch0, Xscratch0, Xscratch1);
    }
    code.STR(Xscratch0, SP, offsetof(StackLayout, cycles_remaining));
}

EmittedBlockInfo EmitArm64(oaknut::CodeGenerator& code, IR::Block block, const EmitConfig& conf) {
    EmittedBlockInfo ebi;

    FpsrManager fpsr_manager{code, conf.state_fpsr_offset};
    RegAlloc reg_alloc{code, fpsr_manager, GPR_ORDER, FPR_ORDER};
    EmitContext ctx{block, reg_alloc, conf, ebi, fpsr_manager};

    ebi.entry_point = code.ptr<CodePtr>();

    if (ctx.block.GetCondition() == IR::Cond::AL) {
        ASSERT(!ctx.block.HasConditionFailedLocation());
    } else {
        ASSERT(ctx.block.HasConditionFailedLocation());

        oaknut::Label pass = EmitA32Cond(code, ctx, ctx.block.GetCondition());
        if (conf.enable_cycle_counting) {
            EmitAddCycles(code, ctx, ctx.block.ConditionFailedCycleCount());
        }
        EmitA32ConditionFailedTerminal(code, ctx);
        code.l(pass);
    }

    for (auto iter = block.begin(); iter != block.end(); ++iter) {
        IR::Inst* inst = &*iter;

        switch (inst->GetOpcode()) {
#define OPCODE(name, type, ...)                    \
    case IR::Opcode::name:                         \
        EmitIR<IR::Opcode::name>(code, ctx, inst); \
        break;
#define A32OPC(name, type, ...)                         \
    case IR::Opcode::A32##name:                         \
        EmitIR<IR::Opcode::A32##name>(code, ctx, inst); \
        break;
#define A64OPC(name, type, ...)                         \
    case IR::Opcode::A64##name:                         \
        EmitIR<IR::Opcode::A64##name>(code, ctx, inst); \
        break;
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
        default:
            ASSERT_FALSE("Invalid opcode: {}", inst->GetOpcode());
            break;
        }

        reg_alloc.UpdateAllUses();
        reg_alloc.AssertAllUnlocked();
    }

    fpsr_manager.Spill();

    reg_alloc.AssertNoMoreUses();

    if (ctx.conf.enable_cycle_counting) {
        EmitAddCycles(code, ctx, block.CycleCount());
    }

    EmitA32Terminal(code, ctx);

    ebi.size = code.ptr<CodePtr>() - ebi.entry_point;
    return ebi;
}

void EmitRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, LinkTarget link_target) {
    ctx.ebi.relocations.emplace_back(Relocation{code.ptr<CodePtr>() - ctx.ebi.entry_point, link_target});
    code.NOP();
}

}  // namespace Dynarmic::Backend::Arm64
