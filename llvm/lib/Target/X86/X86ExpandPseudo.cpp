//===------- X86ExpandPseudo.cpp - Expand pseudo instructions -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that expands pseudo instructions into target
// instructions to allow proper scheduling, if-conversion, other late
// optimizations, or simply the encoding of the instructions.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86FrameLowering.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Passes.h" // For IDs of passes that are preserved.
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

#define DEBUG_TYPE "x86-pseudo"
#define X86_EXPAND_PSEUDO_NAME "X86 pseudo instruction expansion pass"

namespace {
class X86ExpandPseudo : public MachineFunctionPass {
public:
  static char ID;
  X86ExpandPseudo() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addPreservedID(MachineLoopInfoID);
    AU.addPreservedID(MachineDominatorsID);
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  const X86Subtarget *STI = nullptr;
  const X86InstrInfo *TII = nullptr;
  const X86RegisterInfo *TRI = nullptr;
  const X86MachineFunctionInfo *X86FI = nullptr;
  const X86FrameLowering *X86FL = nullptr;

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  StringRef getPassName() const override {
    return "X86 pseudo instruction expansion pass";
  }

private:
  void expandICallBranchFunnel(MachineBasicBlock *MBB,
                               MachineBasicBlock::iterator MBBI);
  void expandCALL_RVMARKER(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
  bool expandMBB(MachineBasicBlock &MBB);

  /// This function expands pseudos which affects control flow.
  /// It is done in separate pass to simplify blocks navigation in main
  /// pass(calling expandMBB).
  bool expandPseudosWhichAffectControlFlow(MachineFunction &MF);

  /// Expand X86::VASTART_SAVE_XMM_REGS into set of xmm copying instructions,
  /// placed into separate block guarded by check for al register(for SystemV
  /// abi).
  void expandVastartSaveXmmRegs(
      MachineBasicBlock *EntryBlk,
      MachineBasicBlock::iterator VAStartPseudoInstr) const;
};
char X86ExpandPseudo::ID = 0;

} // End anonymous namespace.

INITIALIZE_PASS(X86ExpandPseudo, DEBUG_TYPE, X86_EXPAND_PSEUDO_NAME, false,
                false)

void X86ExpandPseudo::expandICallBranchFunnel(
    MachineBasicBlock *MBB, MachineBasicBlock::iterator MBBI) {
  MachineBasicBlock *JTMBB = MBB;
  MachineInstr *JTInst = &*MBBI;
  MachineFunction *MF = MBB->getParent();
  const BasicBlock *BB = MBB->getBasicBlock();
  auto InsPt = MachineFunction::iterator(MBB);
  ++InsPt;

  std::vector<std::pair<MachineBasicBlock *, unsigned>> TargetMBBs;
  const DebugLoc &DL = JTInst->getDebugLoc();
  MachineOperand Selector = JTInst->getOperand(0);
  const GlobalValue *CombinedGlobal = JTInst->getOperand(1).getGlobal();

  auto CmpTarget = [&](unsigned Target) {
    if (Selector.isReg())
      MBB->addLiveIn(Selector.getReg());
    BuildMI(*MBB, MBBI, DL, TII->get(X86::LEA64r), X86::R11)
        .addReg(X86::RIP)
        .addImm(1)
        .addReg(0)
        .addGlobalAddress(CombinedGlobal,
                          JTInst->getOperand(2 + 2 * Target).getImm())
        .addReg(0);
    BuildMI(*MBB, MBBI, DL, TII->get(X86::CMP64rr))
        .add(Selector)
        .addReg(X86::R11);
  };

  auto CreateMBB = [&]() {
    auto *NewMBB = MF->CreateMachineBasicBlock(BB);
    MBB->addSuccessor(NewMBB);
    if (!MBB->isLiveIn(X86::EFLAGS))
      MBB->addLiveIn(X86::EFLAGS);
    return NewMBB;
  };

  auto EmitCondJump = [&](unsigned CC, MachineBasicBlock *ThenMBB) {
    BuildMI(*MBB, MBBI, DL, TII->get(X86::JCC_1)).addMBB(ThenMBB).addImm(CC);

    auto *ElseMBB = CreateMBB();
    MF->insert(InsPt, ElseMBB);
    MBB = ElseMBB;
    MBBI = MBB->end();
  };

  auto EmitCondJumpTarget = [&](unsigned CC, unsigned Target) {
    auto *ThenMBB = CreateMBB();
    TargetMBBs.push_back({ThenMBB, Target});
    EmitCondJump(CC, ThenMBB);
  };

  auto EmitTailCall = [&](unsigned Target) {
    BuildMI(*MBB, MBBI, DL, TII->get(X86::TAILJMPd64))
        .add(JTInst->getOperand(3 + 2 * Target));
  };

  std::function<void(unsigned, unsigned)> EmitBranchFunnel =
      [&](unsigned FirstTarget, unsigned NumTargets) {
    if (NumTargets == 1) {
      EmitTailCall(FirstTarget);
      return;
    }

    if (NumTargets == 2) {
      CmpTarget(FirstTarget + 1);
      EmitCondJumpTarget(X86::COND_B, FirstTarget);
      EmitTailCall(FirstTarget + 1);
      return;
    }

    if (NumTargets < 6) {
      CmpTarget(FirstTarget + 1);
      EmitCondJumpTarget(X86::COND_B, FirstTarget);
      EmitCondJumpTarget(X86::COND_E, FirstTarget + 1);
      EmitBranchFunnel(FirstTarget + 2, NumTargets - 2);
      return;
    }

    auto *ThenMBB = CreateMBB();
    CmpTarget(FirstTarget + (NumTargets / 2));
    EmitCondJump(X86::COND_B, ThenMBB);
    EmitCondJumpTarget(X86::COND_E, FirstTarget + (NumTargets / 2));
    EmitBranchFunnel(FirstTarget + (NumTargets / 2) + 1,
                  NumTargets - (NumTargets / 2) - 1);

    MF->insert(InsPt, ThenMBB);
    MBB = ThenMBB;
    MBBI = MBB->end();
    EmitBranchFunnel(FirstTarget, NumTargets / 2);
  };

  EmitBranchFunnel(0, (JTInst->getNumOperands() - 2) / 2);
  for (auto P : TargetMBBs) {
    MF->insert(InsPt, P.first);
    BuildMI(P.first, DL, TII->get(X86::TAILJMPd64))
        .add(JTInst->getOperand(3 + 2 * P.second));
  }
  JTMBB->erase(JTInst);
}

void X86ExpandPseudo::expandCALL_RVMARKER(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MBBI) {
  // Expand CALL_RVMARKER pseudo to call instruction, followed by the special
  //"movq %rax, %rdi" marker.
  MachineInstr &MI = *MBBI;

  MachineInstr *OriginalCall;
  assert((MI.getOperand(1).isGlobal() || MI.getOperand(1).isReg()) &&
         "invalid operand for regular call");
  unsigned Opc = -1;
  if (MI.getOpcode() == X86::CALL64m_RVMARKER)
    Opc = X86::CALL64m;
  else if (MI.getOpcode() == X86::CALL64r_RVMARKER)
    Opc = X86::CALL64r;
  else if (MI.getOpcode() == X86::CALL64pcrel32_RVMARKER)
    Opc = X86::CALL64pcrel32;
  else
    llvm_unreachable("unexpected opcode");

  OriginalCall = BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(Opc)).getInstr();
  bool RAXImplicitDead = false;
  for (MachineOperand &Op : llvm::drop_begin(MI.operands())) {
    // RAX may be 'implicit dead', if there are no other users of the return
    // value. We introduce a new use, so change it to 'implicit def'.
    if (Op.isReg() && Op.isImplicit() && Op.isDead() &&
        TRI->regsOverlap(Op.getReg(), X86::RAX)) {
      Op.setIsDead(false);
      Op.setIsDef(true);
      RAXImplicitDead = true;
    }
    OriginalCall->addOperand(Op);
  }

  // Emit marker "movq %rax, %rdi".  %rdi is not callee-saved, so it cannot be
  // live across the earlier call. The call to the ObjC runtime function returns
  // the first argument, so the value of %rax is unchanged after the ObjC
  // runtime call. On Windows targets, the runtime call follows the regular
  // x64 calling convention and expects the first argument in %rcx.
  auto TargetReg = STI->getTargetTriple().isOSWindows() ? X86::RCX : X86::RDI;
  auto *Marker = BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(X86::MOV64rr))
                     .addReg(TargetReg, RegState::Define)
                     .addReg(X86::RAX)
                     .getInstr();
  if (MI.shouldUpdateAdditionalCallInfo())
    MBB.getParent()->moveAdditionalCallInfo(&MI, Marker);

  // Emit call to ObjC runtime.
  const uint32_t *RegMask =
      TRI->getCallPreservedMask(*MBB.getParent(), CallingConv::C);
  MachineInstr *RtCall =
      BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(X86::CALL64pcrel32))
          .addGlobalAddress(MI.getOperand(0).getGlobal(), 0, 0)
          .addRegMask(RegMask)
          .addReg(X86::RAX,
                  RegState::Implicit |
                      (RAXImplicitDead ? (RegState::Dead | RegState::Define)
                                       : RegState::Define))
          .getInstr();
  MI.eraseFromParent();

  auto &TM = MBB.getParent()->getTarget();
  // On Darwin platforms, wrap the expanded sequence in a bundle to prevent
  // later optimizations from breaking up the sequence.
  if (TM.getTargetTriple().isOSDarwin())
    finalizeBundle(MBB, OriginalCall->getIterator(),
                   std::next(RtCall->getIterator()));
}

/// If \p MBBI is a pseudo instruction, this method expands
/// it to the corresponding (sequence of) actual instruction(s).
/// \returns true if \p MBBI has been expanded.
bool X86ExpandPseudo::expandMI(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MBBI) {
  MachineInstr &MI = *MBBI;
  unsigned Opcode = MI.getOpcode();
  const DebugLoc &DL = MBBI->getDebugLoc();
#define GET_EGPR_IF_ENABLED(OPC) (STI->hasEGPR() ? OPC##_EVEX : OPC)
  switch (Opcode) {
  default:
    return false;
  case X86::TCRETURNdi:
  case X86::TCRETURNdicc:
  case X86::TCRETURNri:
  case X86::TCRETURNmi:
  case X86::TCRETURNdi64:
  case X86::TCRETURNdi64cc:
  case X86::TCRETURNri64:
  case X86::TCRETURNri64_ImpCall:
  case X86::TCRETURNmi64: {
    bool isMem = Opcode == X86::TCRETURNmi || Opcode == X86::TCRETURNmi64;
    MachineOperand &JumpTarget = MBBI->getOperand(0);
    MachineOperand &StackAdjust = MBBI->getOperand(isMem ? X86::AddrNumOperands
                                                         : 1);
    assert(StackAdjust.isImm() && "Expecting immediate value.");

    // Adjust stack pointer.
    int StackAdj = StackAdjust.getImm();
    int MaxTCDelta = X86FI->getTCReturnAddrDelta();
    int64_t Offset = 0;
    assert(MaxTCDelta <= 0 && "MaxTCDelta should never be positive");

    // Incoporate the retaddr area.
    Offset = StackAdj - MaxTCDelta;
    assert(Offset >= 0 && "Offset should never be negative");

    if (Opcode == X86::TCRETURNdicc || Opcode == X86::TCRETURNdi64cc) {
      assert(Offset == 0 && "Conditional tail call cannot adjust the stack.");
    }

    if (Offset) {
      // Check for possible merge with preceding ADD instruction.
      Offset = X86FL->mergeSPAdd(MBB, MBBI, Offset, true);
      X86FL->emitSPUpdate(MBB, MBBI, DL, Offset, /*InEpilogue=*/true);
    }

    // Use this predicate to set REX prefix for X86_64 targets.
    bool IsX64 = STI->isTargetWin64() || STI->isTargetUEFI64();
    // Jump to label or value in register.
    if (Opcode == X86::TCRETURNdi || Opcode == X86::TCRETURNdicc ||
        Opcode == X86::TCRETURNdi64 || Opcode == X86::TCRETURNdi64cc) {
      unsigned Op;
      switch (Opcode) {
      case X86::TCRETURNdi:
        Op = X86::TAILJMPd;
        break;
      case X86::TCRETURNdicc:
        Op = X86::TAILJMPd_CC;
        break;
      case X86::TCRETURNdi64cc:
        assert(!MBB.getParent()->hasWinCFI() &&
               "Conditional tail calls confuse "
               "the Win64 unwinder.");
        Op = X86::TAILJMPd64_CC;
        break;
      default:
        // Note: Win64 uses REX prefixes indirect jumps out of functions, but
        // not direct ones.
        Op = X86::TAILJMPd64;
        break;
      }
      MachineInstrBuilder MIB = BuildMI(MBB, MBBI, DL, TII->get(Op));
      if (JumpTarget.isGlobal()) {
        MIB.addGlobalAddress(JumpTarget.getGlobal(), JumpTarget.getOffset(),
                             JumpTarget.getTargetFlags());
      } else {
        assert(JumpTarget.isSymbol());
        MIB.addExternalSymbol(JumpTarget.getSymbolName(),
                              JumpTarget.getTargetFlags());
      }
      if (Op == X86::TAILJMPd_CC || Op == X86::TAILJMPd64_CC) {
        MIB.addImm(MBBI->getOperand(2).getImm());
      }

    } else if (Opcode == X86::TCRETURNmi || Opcode == X86::TCRETURNmi64) {
      unsigned Op = (Opcode == X86::TCRETURNmi)
                        ? X86::TAILJMPm
                        : (IsX64 ? X86::TAILJMPm64_REX : X86::TAILJMPm64);
      MachineInstrBuilder MIB = BuildMI(MBB, MBBI, DL, TII->get(Op));
      for (unsigned i = 0; i != X86::AddrNumOperands; ++i)
        MIB.add(MBBI->getOperand(i));
    } else if ((Opcode == X86::TCRETURNri64) ||
               (Opcode == X86::TCRETURNri64_ImpCall)) {
      JumpTarget.setIsKill();
      BuildMI(MBB, MBBI, DL,
              TII->get(IsX64 ? X86::TAILJMPr64_REX : X86::TAILJMPr64))
          .add(JumpTarget);
    } else {
      assert(!IsX64 && "Win64 and UEFI64 require REX for indirect jumps.");
      JumpTarget.setIsKill();
      BuildMI(MBB, MBBI, DL, TII->get(X86::TAILJMPr))
          .add(JumpTarget);
    }

    MachineInstr &NewMI = *std::prev(MBBI);
    NewMI.copyImplicitOps(*MBBI->getParent()->getParent(), *MBBI);
    NewMI.setCFIType(*MBB.getParent(), MI.getCFIType());

    // Update the call info.
    if (MBBI->isCandidateForAdditionalCallInfo())
      MBB.getParent()->moveAdditionalCallInfo(&*MBBI, &NewMI);

    // Delete the pseudo instruction TCRETURN.
    MBB.erase(MBBI);

    return true;
  }
  case X86::EH_RETURN:
  case X86::EH_RETURN64: {
    MachineOperand &DestAddr = MBBI->getOperand(0);
    assert(DestAddr.isReg() && "Offset should be in register!");
    const bool Uses64BitFramePtr = STI->isTarget64BitLP64();
    Register StackPtr = TRI->getStackRegister();
    BuildMI(MBB, MBBI, DL,
            TII->get(Uses64BitFramePtr ? X86::MOV64rr : X86::MOV32rr), StackPtr)
        .addReg(DestAddr.getReg());
    // The EH_RETURN pseudo is really removed during the MC Lowering.
    return true;
  }
  case X86::IRET: {
    // Adjust stack to erase error code
    int64_t StackAdj = MBBI->getOperand(0).getImm();
    X86FL->emitSPUpdate(MBB, MBBI, DL, StackAdj, true);
    // Replace pseudo with machine iret
    unsigned RetOp = STI->is64Bit() ? X86::IRET64 : X86::IRET32;
    // Use UIRET if UINTR is present (except for building kernel)
    if (STI->is64Bit() && STI->hasUINTR() &&
        MBB.getParent()->getTarget().getCodeModel() != CodeModel::Kernel)
      RetOp = X86::UIRET;
    BuildMI(MBB, MBBI, DL, TII->get(RetOp));
    MBB.erase(MBBI);
    return true;
  }
  case X86::RET: {
    // Adjust stack to erase error code
    int64_t StackAdj = MBBI->getOperand(0).getImm();
    MachineInstrBuilder MIB;
    if (StackAdj == 0) {
      MIB = BuildMI(MBB, MBBI, DL,
                    TII->get(STI->is64Bit() ? X86::RET64 : X86::RET32));
    } else if (isUInt<16>(StackAdj)) {
      MIB = BuildMI(MBB, MBBI, DL,
                    TII->get(STI->is64Bit() ? X86::RETI64 : X86::RETI32))
                .addImm(StackAdj);
    } else {
      assert(!STI->is64Bit() &&
             "shouldn't need to do this for x86_64 targets!");
      // A ret can only handle immediates as big as 2**16-1.  If we need to pop
      // off bytes before the return address, we must do it manually.
      BuildMI(MBB, MBBI, DL, TII->get(X86::POP32r)).addReg(X86::ECX, RegState::Define);
      X86FL->emitSPUpdate(MBB, MBBI, DL, StackAdj, /*InEpilogue=*/true);
      BuildMI(MBB, MBBI, DL, TII->get(X86::PUSH32r)).addReg(X86::ECX);
      MIB = BuildMI(MBB, MBBI, DL, TII->get(X86::RET32));
    }
    for (unsigned I = 1, E = MBBI->getNumOperands(); I != E; ++I)
      MIB.add(MBBI->getOperand(I));
    MBB.erase(MBBI);
    return true;
  }
  case X86::LCMPXCHG16B_SAVE_RBX: {
    // Perform the following transformation.
    // SaveRbx = pseudocmpxchg Addr, <4 opds for the address>, InArg, SaveRbx
    // =>
    // RBX = InArg
    // actualcmpxchg Addr
    // RBX = SaveRbx
    const MachineOperand &InArg = MBBI->getOperand(6);
    Register SaveRbx = MBBI->getOperand(7).getReg();

    // Copy the input argument of the pseudo into the argument of the
    // actual instruction.
    // NOTE: We don't copy the kill flag since the input might be the same reg
    // as one of the other operands of LCMPXCHG16B.
    TII->copyPhysReg(MBB, MBBI, DL, X86::RBX, InArg.getReg(), false);
    // Create the actual instruction.
    MachineInstr *NewInstr = BuildMI(MBB, MBBI, DL, TII->get(X86::LCMPXCHG16B));
    // Copy the operands related to the address. If we access a frame variable,
    // we need to replace the RBX base with SaveRbx, as RBX has another value.
    const MachineOperand &Base = MBBI->getOperand(1);
    if (Base.getReg() == X86::RBX || Base.getReg() == X86::EBX)
      NewInstr->addOperand(MachineOperand::CreateReg(
          Base.getReg() == X86::RBX
              ? SaveRbx
              : Register(TRI->getSubReg(SaveRbx, X86::sub_32bit)),
          /*IsDef=*/false));
    else
      NewInstr->addOperand(Base);
    for (unsigned Idx = 1 + 1; Idx < 1 + X86::AddrNumOperands; ++Idx)
      NewInstr->addOperand(MBBI->getOperand(Idx));
    // Finally, restore the value of RBX.
    TII->copyPhysReg(MBB, MBBI, DL, X86::RBX, SaveRbx,
                     /*SrcIsKill*/ true);

    // Delete the pseudo.
    MBBI->eraseFromParent();
    return true;
  }
  // Loading/storing mask pairs requires two kmov operations. The second one of
  // these needs a 2 byte displacement relative to the specified address (with
  // 32 bit spill size). The pairs of 1bit masks up to 16 bit masks all use the
  // same spill size, they all are stored using MASKPAIR16STORE, loaded using
  // MASKPAIR16LOAD.
  //
  // The displacement value might wrap around in theory, thus the asserts in
  // both cases.
  case X86::MASKPAIR16LOAD: {
    int64_t Disp = MBBI->getOperand(1 + X86::AddrDisp).getImm();
    assert(Disp >= 0 && Disp <= INT32_MAX - 2 && "Unexpected displacement");
    Register Reg = MBBI->getOperand(0).getReg();
    bool DstIsDead = MBBI->getOperand(0).isDead();
    Register Reg0 = TRI->getSubReg(Reg, X86::sub_mask_0);
    Register Reg1 = TRI->getSubReg(Reg, X86::sub_mask_1);

    auto MIBLo =
        BuildMI(MBB, MBBI, DL, TII->get(GET_EGPR_IF_ENABLED(X86::KMOVWkm)))
            .addReg(Reg0, RegState::Define | getDeadRegState(DstIsDead));
    auto MIBHi =
        BuildMI(MBB, MBBI, DL, TII->get(GET_EGPR_IF_ENABLED(X86::KMOVWkm)))
            .addReg(Reg1, RegState::Define | getDeadRegState(DstIsDead));

    for (int i = 0; i < X86::AddrNumOperands; ++i) {
      MIBLo.add(MBBI->getOperand(1 + i));
      if (i == X86::AddrDisp)
        MIBHi.addImm(Disp + 2);
      else
        MIBHi.add(MBBI->getOperand(1 + i));
    }

    // Split the memory operand, adjusting the offset and size for the halves.
    MachineMemOperand *OldMMO = MBBI->memoperands().front();
    MachineFunction *MF = MBB.getParent();
    MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, 2);
    MachineMemOperand *MMOHi = MF->getMachineMemOperand(OldMMO, 2, 2);

    MIBLo.setMemRefs(MMOLo);
    MIBHi.setMemRefs(MMOHi);

    // Delete the pseudo.
    MBB.erase(MBBI);
    return true;
  }
  case X86::MASKPAIR16STORE: {
    int64_t Disp = MBBI->getOperand(X86::AddrDisp).getImm();
    assert(Disp >= 0 && Disp <= INT32_MAX - 2 && "Unexpected displacement");
    Register Reg = MBBI->getOperand(X86::AddrNumOperands).getReg();
    bool SrcIsKill = MBBI->getOperand(X86::AddrNumOperands).isKill();
    Register Reg0 = TRI->getSubReg(Reg, X86::sub_mask_0);
    Register Reg1 = TRI->getSubReg(Reg, X86::sub_mask_1);

    auto MIBLo =
        BuildMI(MBB, MBBI, DL, TII->get(GET_EGPR_IF_ENABLED(X86::KMOVWmk)));
    auto MIBHi =
        BuildMI(MBB, MBBI, DL, TII->get(GET_EGPR_IF_ENABLED(X86::KMOVWmk)));

    for (int i = 0; i < X86::AddrNumOperands; ++i) {
      MIBLo.add(MBBI->getOperand(i));
      if (i == X86::AddrDisp)
        MIBHi.addImm(Disp + 2);
      else
        MIBHi.add(MBBI->getOperand(i));
    }
    MIBLo.addReg(Reg0, getKillRegState(SrcIsKill));
    MIBHi.addReg(Reg1, getKillRegState(SrcIsKill));

    // Split the memory operand, adjusting the offset and size for the halves.
    MachineMemOperand *OldMMO = MBBI->memoperands().front();
    MachineFunction *MF = MBB.getParent();
    MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, 2);
    MachineMemOperand *MMOHi = MF->getMachineMemOperand(OldMMO, 2, 2);

    MIBLo.setMemRefs(MMOLo);
    MIBHi.setMemRefs(MMOHi);

    // Delete the pseudo.
    MBB.erase(MBBI);
    return true;
  }
  case X86::MWAITX_SAVE_RBX: {
    // Perform the following transformation.
    // SaveRbx = pseudomwaitx InArg, SaveRbx
    // =>
    // [E|R]BX = InArg
    // actualmwaitx
    // [E|R]BX = SaveRbx
    const MachineOperand &InArg = MBBI->getOperand(1);
    // Copy the input argument of the pseudo into the argument of the
    // actual instruction.
    TII->copyPhysReg(MBB, MBBI, DL, X86::EBX, InArg.getReg(), InArg.isKill());
    // Create the actual instruction.
    BuildMI(MBB, MBBI, DL, TII->get(X86::MWAITXrrr));
    // Finally, restore the value of RBX.
    Register SaveRbx = MBBI->getOperand(2).getReg();
    TII->copyPhysReg(MBB, MBBI, DL, X86::RBX, SaveRbx, /*SrcIsKill*/ true);
    // Delete the pseudo.
    MBBI->eraseFromParent();
    return true;
  }
  case TargetOpcode::ICALL_BRANCH_FUNNEL:
    expandICallBranchFunnel(&MBB, MBBI);
    return true;
  case X86::PLDTILECFGV: {
    MI.setDesc(TII->get(GET_EGPR_IF_ENABLED(X86::LDTILECFG)));
    return true;
  }
  case X86::PTILELOADDV:
  case X86::PTILELOADDT1V:
  case X86::PTILELOADDRSV:
  case X86::PTILELOADDRST1V:
  case X86::PTCVTROWD2PSrreV:
  case X86::PTCVTROWD2PSrriV:
  case X86::PTCVTROWPS2BF16HrreV:
  case X86::PTCVTROWPS2BF16HrriV:
  case X86::PTCVTROWPS2BF16LrreV:
  case X86::PTCVTROWPS2BF16LrriV:
  case X86::PTCVTROWPS2PHHrreV:
  case X86::PTCVTROWPS2PHHrriV:
  case X86::PTCVTROWPS2PHLrreV:
  case X86::PTCVTROWPS2PHLrriV:
  case X86::PTILEMOVROWrreV:
  case X86::PTILEMOVROWrriV: {
    for (unsigned i = 2; i > 0; --i)
      MI.removeOperand(i);
    unsigned Opc;
    switch (Opcode) {
    case X86::PTILELOADDRSV:
      Opc = GET_EGPR_IF_ENABLED(X86::TILELOADDRS);
      break;
    case X86::PTILELOADDRST1V:
      Opc = GET_EGPR_IF_ENABLED(X86::TILELOADDRST1);
      break;
    case X86::PTILELOADDV:
      Opc = GET_EGPR_IF_ENABLED(X86::TILELOADD);
      break;
    case X86::PTILELOADDT1V:
      Opc = GET_EGPR_IF_ENABLED(X86::TILELOADDT1);
      break;
    case X86::PTCVTROWD2PSrreV:
      Opc = X86::TCVTROWD2PSrre;
      break;
    case X86::PTCVTROWD2PSrriV:
      Opc = X86::TCVTROWD2PSrri;
      break;
    case X86::PTCVTROWPS2BF16HrreV:
      Opc = X86::TCVTROWPS2BF16Hrre;
      break;
    case X86::PTCVTROWPS2BF16HrriV:
      Opc = X86::TCVTROWPS2BF16Hrri;
      break;
    case X86::PTCVTROWPS2BF16LrreV:
      Opc = X86::TCVTROWPS2BF16Lrre;
      break;
    case X86::PTCVTROWPS2BF16LrriV:
      Opc = X86::TCVTROWPS2BF16Lrri;
      break;
    case X86::PTCVTROWPS2PHHrreV:
      Opc = X86::TCVTROWPS2PHHrre;
      break;
    case X86::PTCVTROWPS2PHHrriV:
      Opc = X86::TCVTROWPS2PHHrri;
      break;
    case X86::PTCVTROWPS2PHLrreV:
      Opc = X86::TCVTROWPS2PHLrre;
      break;
    case X86::PTCVTROWPS2PHLrriV:
      Opc = X86::TCVTROWPS2PHLrri;
      break;
    case X86::PTILEMOVROWrreV:
      Opc = X86::TILEMOVROWrre;
      break;
    case X86::PTILEMOVROWrriV:
      Opc = X86::TILEMOVROWrri;
      break;
    default:
      llvm_unreachable("Unexpected Opcode");
    }
    MI.setDesc(TII->get(Opc));
    return true;
  }
  // TILEPAIRLOAD is just for TILEPair spill, we don't have corresponding
  // AMX instruction to support it. So, split it to 2 load instructions:
  // "TILEPAIRLOAD TMM0:TMM1, Base, Scale, Index, Offset, Segment" -->
  // "TILELOAD TMM0, Base, Scale, Index, Offset, Segment" +
  // "TILELOAD TMM1, Base, Scale, Index, Offset + TMM_SIZE, Segment"
  case X86::PTILEPAIRLOAD: {
    int64_t Disp = MBBI->getOperand(1 + X86::AddrDisp).getImm();
    Register TReg = MBBI->getOperand(0).getReg();
    bool DstIsDead = MBBI->getOperand(0).isDead();
    Register TReg0 = TRI->getSubReg(TReg, X86::sub_t0);
    Register TReg1 = TRI->getSubReg(TReg, X86::sub_t1);
    unsigned TmmSize = TRI->getRegSizeInBits(X86::TILERegClass) / 8;

    MachineInstrBuilder MIBLo =
        BuildMI(MBB, MBBI, DL, TII->get(X86::TILELOADD))
            .addReg(TReg0, RegState::Define | getDeadRegState(DstIsDead));
    MachineInstrBuilder MIBHi =
        BuildMI(MBB, MBBI, DL, TII->get(X86::TILELOADD))
            .addReg(TReg1, RegState::Define | getDeadRegState(DstIsDead));

    for (int i = 0; i < X86::AddrNumOperands; ++i) {
      MIBLo.add(MBBI->getOperand(1 + i));
      if (i == X86::AddrDisp)
        MIBHi.addImm(Disp + TmmSize);
      else
        MIBHi.add(MBBI->getOperand(1 + i));
    }

    // Make sure the first stride reg used in first tileload is alive.
    MachineOperand &Stride =
        MIBLo.getInstr()->getOperand(1 + X86::AddrIndexReg);
    Stride.setIsKill(false);

    // Split the memory operand, adjusting the offset and size for the halves.
    MachineMemOperand *OldMMO = MBBI->memoperands().front();
    MachineFunction *MF = MBB.getParent();
    MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, TmmSize);
    MachineMemOperand *MMOHi =
        MF->getMachineMemOperand(OldMMO, TmmSize, TmmSize);

    MIBLo.setMemRefs(MMOLo);
    MIBHi.setMemRefs(MMOHi);

    // Delete the pseudo.
    MBB.erase(MBBI);
    return true;
  }
  // Similar with TILEPAIRLOAD, TILEPAIRSTORE is just for TILEPair spill, no
  // corresponding AMX instruction to support it. So, split it too:
  // "TILEPAIRSTORE Base, Scale, Index, Offset, Segment, TMM0:TMM1" -->
  // "TILESTORE Base, Scale, Index, Offset, Segment, TMM0" +
  // "TILESTORE Base, Scale, Index, Offset + TMM_SIZE, Segment, TMM1"
  case X86::PTILEPAIRSTORE: {
    int64_t Disp = MBBI->getOperand(X86::AddrDisp).getImm();
    Register TReg = MBBI->getOperand(X86::AddrNumOperands).getReg();
    bool SrcIsKill = MBBI->getOperand(X86::AddrNumOperands).isKill();
    Register TReg0 = TRI->getSubReg(TReg, X86::sub_t0);
    Register TReg1 = TRI->getSubReg(TReg, X86::sub_t1);
    unsigned TmmSize = TRI->getRegSizeInBits(X86::TILERegClass) / 8;

    MachineInstrBuilder MIBLo =
        BuildMI(MBB, MBBI, DL, TII->get(X86::TILESTORED));
    MachineInstrBuilder MIBHi =
        BuildMI(MBB, MBBI, DL, TII->get(X86::TILESTORED));

    for (int i = 0; i < X86::AddrNumOperands; ++i) {
      MIBLo.add(MBBI->getOperand(i));
      if (i == X86::AddrDisp)
        MIBHi.addImm(Disp + TmmSize);
      else
        MIBHi.add(MBBI->getOperand(i));
    }
    MIBLo.addReg(TReg0, getKillRegState(SrcIsKill));
    MIBHi.addReg(TReg1, getKillRegState(SrcIsKill));

    // Make sure the first stride reg used in first tilestore is alive.
    MachineOperand &Stride = MIBLo.getInstr()->getOperand(X86::AddrIndexReg);
    Stride.setIsKill(false);

    // Split the memory operand, adjusting the offset and size for the halves.
    MachineMemOperand *OldMMO = MBBI->memoperands().front();
    MachineFunction *MF = MBB.getParent();
    MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, TmmSize);
    MachineMemOperand *MMOHi =
        MF->getMachineMemOperand(OldMMO, TmmSize, TmmSize);

    MIBLo.setMemRefs(MMOLo);
    MIBHi.setMemRefs(MMOHi);

    // Delete the pseudo.
    MBB.erase(MBBI);
    return true;
  }
  case X86::PT2RPNTLVWZ0V:
  case X86::PT2RPNTLVWZ0T1V:
  case X86::PT2RPNTLVWZ1V:
  case X86::PT2RPNTLVWZ1T1V:
  case X86::PT2RPNTLVWZ0RSV:
  case X86::PT2RPNTLVWZ0RST1V:
  case X86::PT2RPNTLVWZ1RSV:
  case X86::PT2RPNTLVWZ1RST1V: {
    for (unsigned i = 3; i > 0; --i)
      MI.removeOperand(i);
    unsigned Opc;
    switch (Opcode) {
    case X86::PT2RPNTLVWZ0V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ0);
      break;
    case X86::PT2RPNTLVWZ0T1V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ0T1);
      break;
    case X86::PT2RPNTLVWZ1V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ1);
      break;
    case X86::PT2RPNTLVWZ1T1V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ1T1);
      break;
    case X86::PT2RPNTLVWZ0RSV:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ0RS);
      break;
    case X86::PT2RPNTLVWZ0RST1V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ0RST1);
      break;
    case X86::PT2RPNTLVWZ1RSV:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ1RS);
      break;
    case X86::PT2RPNTLVWZ1RST1V:
      Opc = GET_EGPR_IF_ENABLED(X86::T2RPNTLVWZ1RST1);
      break;
    default:
      llvm_unreachable("Impossible Opcode!");
    }
    MI.setDesc(TII->get(Opc));
    return true;
  }
  case X86::PTTRANSPOSEDV:
  case X86::PTCONJTFP16V: {
    for (int i = 2; i > 0; --i)
      MI.removeOperand(i);
    MI.setDesc(TII->get(Opcode == X86::PTTRANSPOSEDV ? X86::TTRANSPOSED
                                                     : X86::TCONJTFP16));
    return true;
  }
  case X86::PTCMMIMFP16PSV:
  case X86::PTCMMRLFP16PSV:
  case X86::PTDPBSSDV:
  case X86::PTDPBSUDV:
  case X86::PTDPBUSDV:
  case X86::PTDPBUUDV:
  case X86::PTDPBF16PSV:
  case X86::PTDPFP16PSV:
  case X86::PTTDPBF16PSV:
  case X86::PTTDPFP16PSV:
  case X86::PTTCMMIMFP16PSV:
  case X86::PTTCMMRLFP16PSV:
  case X86::PTCONJTCMMIMFP16PSV:
  case X86::PTMMULTF32PSV:
  case X86::PTTMMULTF32PSV:
  case X86::PTDPBF8PSV:
  case X86::PTDPBHF8PSV:
  case X86::PTDPHBF8PSV:
  case X86::PTDPHF8PSV: {
    MI.untieRegOperand(4);
    for (unsigned i = 3; i > 0; --i)
      MI.removeOperand(i);
    unsigned Opc;
    switch (Opcode) {
    case X86::PTCMMIMFP16PSV:  Opc = X86::TCMMIMFP16PS; break;
    case X86::PTCMMRLFP16PSV:  Opc = X86::TCMMRLFP16PS; break;
    case X86::PTDPBSSDV:   Opc = X86::TDPBSSD; break;
    case X86::PTDPBSUDV:   Opc = X86::TDPBSUD; break;
    case X86::PTDPBUSDV:   Opc = X86::TDPBUSD; break;
    case X86::PTDPBUUDV:   Opc = X86::TDPBUUD; break;
    case X86::PTDPBF16PSV: Opc = X86::TDPBF16PS; break;
    case X86::PTDPFP16PSV: Opc = X86::TDPFP16PS; break;
    case X86::PTTDPBF16PSV:
      Opc = X86::TTDPBF16PS;
      break;
    case X86::PTTDPFP16PSV:
      Opc = X86::TTDPFP16PS;
      break;
    case X86::PTTCMMIMFP16PSV:
      Opc = X86::TTCMMIMFP16PS;
      break;
    case X86::PTTCMMRLFP16PSV:
      Opc = X86::TTCMMRLFP16PS;
      break;
    case X86::PTCONJTCMMIMFP16PSV:
      Opc = X86::TCONJTCMMIMFP16PS;
      break;
    case X86::PTMMULTF32PSV:
      Opc = X86::TMMULTF32PS;
      break;
    case X86::PTTMMULTF32PSV:
      Opc = X86::TTMMULTF32PS;
      break;
    case X86::PTDPBF8PSV:
      Opc = X86::TDPBF8PS;
      break;
    case X86::PTDPBHF8PSV:
      Opc = X86::TDPBHF8PS;
      break;
    case X86::PTDPHBF8PSV:
      Opc = X86::TDPHBF8PS;
      break;
    case X86::PTDPHF8PSV:
      Opc = X86::TDPHF8PS;
      break;

    default:
      llvm_unreachable("Unexpected Opcode");
    }
    MI.setDesc(TII->get(Opc));
    MI.tieOperands(0, 1);
    return true;
  }
  case X86::PTILESTOREDV: {
    for (int i = 1; i >= 0; --i)
      MI.removeOperand(i);
    MI.setDesc(TII->get(GET_EGPR_IF_ENABLED(X86::TILESTORED)));
    return true;
  }
#undef GET_EGPR_IF_ENABLED
  case X86::PTILEZEROV: {
    for (int i = 2; i > 0; --i) // Remove row, col
      MI.removeOperand(i);
    MI.setDesc(TII->get(X86::TILEZERO));
    return true;
  }
  case X86::CALL64pcrel32_RVMARKER:
  case X86::CALL64r_RVMARKER:
  case X86::CALL64m_RVMARKER:
    expandCALL_RVMARKER(MBB, MBBI);
    return true;
  case X86::CALL64r_ImpCall:
    MI.setDesc(TII->get(X86::CALL64r));
    return true;
  case X86::ADD32mi_ND:
  case X86::ADD64mi32_ND:
  case X86::SUB32mi_ND:
  case X86::SUB64mi32_ND:
  case X86::AND32mi_ND:
  case X86::AND64mi32_ND:
  case X86::OR32mi_ND:
  case X86::OR64mi32_ND:
  case X86::XOR32mi_ND:
  case X86::XOR64mi32_ND:
  case X86::ADC32mi_ND:
  case X86::ADC64mi32_ND:
  case X86::SBB32mi_ND:
  case X86::SBB64mi32_ND: {
    // It's possible for an EVEX-encoded legacy instruction to reach the 15-byte
    // instruction length limit: 4 bytes of EVEX prefix + 1 byte of opcode + 1
    // byte of ModRM + 1 byte of SIB + 4 bytes of displacement + 4 bytes of
    // immediate = 15 bytes in total, e.g.
    //
    //  subq    $184, %fs:257(%rbx, %rcx), %rax
    //
    // In such a case, no additional (ADSIZE or segment override) prefix can be
    // used. To resolve the issue, we split the “long” instruction into 2
    // instructions:
    //
    //  movq %fs:257(%rbx, %rcx)，%rax
    //  subq $184, %rax
    //
    //  Therefore we consider the OPmi_ND to be a pseudo instruction to some
    //  extent.
    const MachineOperand &ImmOp =
        MI.getOperand(MI.getNumExplicitOperands() - 1);
    // If the immediate is a expr, conservatively estimate 4 bytes.
    if (ImmOp.isImm() && isInt<8>(ImmOp.getImm()))
      return false;
    int MemOpNo = X86::getFirstAddrOperandIdx(MI);
    const MachineOperand &DispOp = MI.getOperand(MemOpNo + X86::AddrDisp);
    Register Base = MI.getOperand(MemOpNo + X86::AddrBaseReg).getReg();
    // If the displacement is a expr, conservatively estimate 4 bytes.
    if (Base && DispOp.isImm() && isInt<8>(DispOp.getImm()))
      return false;
    // There can only be one of three: SIB, segment override register, ADSIZE
    Register Index = MI.getOperand(MemOpNo + X86::AddrIndexReg).getReg();
    unsigned Count = !!MI.getOperand(MemOpNo + X86::AddrSegmentReg).getReg();
    if (X86II::needSIB(Base, Index, /*In64BitMode=*/true))
      ++Count;
    if (X86MCRegisterClasses[X86::GR32RegClassID].contains(Base) ||
        X86MCRegisterClasses[X86::GR32RegClassID].contains(Index))
      ++Count;
    if (Count < 2)
      return false;
    unsigned Opc, LoadOpc;
    switch (Opcode) {
#define MI_TO_RI(OP)                                                           \
  case X86::OP##32mi_ND:                                                       \
    Opc = X86::OP##32ri;                                                       \
    LoadOpc = X86::MOV32rm;                                                    \
    break;                                                                     \
  case X86::OP##64mi32_ND:                                                     \
    Opc = X86::OP##64ri32;                                                     \
    LoadOpc = X86::MOV64rm;                                                    \
    break;

    default:
      llvm_unreachable("Unexpected Opcode");
      MI_TO_RI(ADD);
      MI_TO_RI(SUB);
      MI_TO_RI(AND);
      MI_TO_RI(OR);
      MI_TO_RI(XOR);
      MI_TO_RI(ADC);
      MI_TO_RI(SBB);
#undef MI_TO_RI
    }
    // Insert OPri.
    Register DestReg = MI.getOperand(0).getReg();
    BuildMI(MBB, std::next(MBBI), DL, TII->get(Opc), DestReg)
        .addReg(DestReg)
        .add(ImmOp);
    // Change OPmi_ND to MOVrm.
    for (unsigned I = MI.getNumImplicitOperands() + 1; I != 0; --I)
      MI.removeOperand(MI.getNumOperands() - 1);
    MI.setDesc(TII->get(LoadOpc));
    return true;
  }
  }
  llvm_unreachable("Previous switch has a fallthrough?");
}

// This function creates additional block for storing varargs guarded
// registers. It adds check for %al into entry block, to skip
// GuardedRegsBlk if xmm registers should not be stored.
//
//     EntryBlk[VAStartPseudoInstr]     EntryBlk
//        |                              |     .
//        |                              |        .
//        |                              |   GuardedRegsBlk
//        |                      =>      |        .
//        |                              |     .
//        |                             TailBlk
//        |                              |
//        |                              |
//
void X86ExpandPseudo::expandVastartSaveXmmRegs(
    MachineBasicBlock *EntryBlk,
    MachineBasicBlock::iterator VAStartPseudoInstr) const {
  assert(VAStartPseudoInstr->getOpcode() == X86::VASTART_SAVE_XMM_REGS);

  MachineFunction *Func = EntryBlk->getParent();
  const TargetInstrInfo *TII = STI->getInstrInfo();
  const DebugLoc &DL = VAStartPseudoInstr->getDebugLoc();
  Register CountReg = VAStartPseudoInstr->getOperand(0).getReg();

  // Calculate liveins for newly created blocks.
  LivePhysRegs LiveRegs(*STI->getRegisterInfo());
  SmallVector<std::pair<MCPhysReg, const MachineOperand *>, 8> Clobbers;

  LiveRegs.addLiveIns(*EntryBlk);
  for (MachineInstr &MI : EntryBlk->instrs()) {
    if (MI.getOpcode() == VAStartPseudoInstr->getOpcode())
      break;

    LiveRegs.stepForward(MI, Clobbers);
  }

  // Create the new basic blocks. One block contains all the XMM stores,
  // and another block is the final destination regardless of whether any
  // stores were performed.
  const BasicBlock *LLVMBlk = EntryBlk->getBasicBlock();
  MachineFunction::iterator EntryBlkIter = ++EntryBlk->getIterator();
  MachineBasicBlock *GuardedRegsBlk = Func->CreateMachineBasicBlock(LLVMBlk);
  MachineBasicBlock *TailBlk = Func->CreateMachineBasicBlock(LLVMBlk);
  Func->insert(EntryBlkIter, GuardedRegsBlk);
  Func->insert(EntryBlkIter, TailBlk);

  // Transfer the remainder of EntryBlk and its successor edges to TailBlk.
  TailBlk->splice(TailBlk->begin(), EntryBlk,
                  std::next(MachineBasicBlock::iterator(VAStartPseudoInstr)),
                  EntryBlk->end());
  TailBlk->transferSuccessorsAndUpdatePHIs(EntryBlk);

  uint64_t FrameOffset = VAStartPseudoInstr->getOperand(4).getImm();
  uint64_t VarArgsRegsOffset = VAStartPseudoInstr->getOperand(6).getImm();

  // TODO: add support for YMM and ZMM here.
  unsigned MOVOpc = STI->hasAVX() ? X86::VMOVAPSmr : X86::MOVAPSmr;

  // In the XMM save block, save all the XMM argument registers.
  for (int64_t OpndIdx = 7, RegIdx = 0;
       OpndIdx < VAStartPseudoInstr->getNumOperands() - 1;
       OpndIdx++, RegIdx++) {
    auto NewMI = BuildMI(GuardedRegsBlk, DL, TII->get(MOVOpc));
    for (int i = 0; i < X86::AddrNumOperands; ++i) {
      if (i == X86::AddrDisp)
        NewMI.addImm(FrameOffset + VarArgsRegsOffset + RegIdx * 16);
      else
        NewMI.add(VAStartPseudoInstr->getOperand(i + 1));
    }
    NewMI.addReg(VAStartPseudoInstr->getOperand(OpndIdx).getReg());
    assert(VAStartPseudoInstr->getOperand(OpndIdx).getReg().isPhysical());
  }

  // The original block will now fall through to the GuardedRegsBlk.
  EntryBlk->addSuccessor(GuardedRegsBlk);
  // The GuardedRegsBlk will fall through to the TailBlk.
  GuardedRegsBlk->addSuccessor(TailBlk);

  if (!STI->isCallingConvWin64(Func->getFunction().getCallingConv())) {
    // If %al is 0, branch around the XMM save block.
    BuildMI(EntryBlk, DL, TII->get(X86::TEST8rr))
        .addReg(CountReg)
        .addReg(CountReg);
    BuildMI(EntryBlk, DL, TII->get(X86::JCC_1))
        .addMBB(TailBlk)
        .addImm(X86::COND_E);
    EntryBlk->addSuccessor(TailBlk);
  }

  // Add liveins to the created block.
  addLiveIns(*GuardedRegsBlk, LiveRegs);
  addLiveIns(*TailBlk, LiveRegs);

  // Delete the pseudo.
  VAStartPseudoInstr->eraseFromParent();
}

/// Expand all pseudo instructions contained in \p MBB.
/// \returns true if any expansion occurred for \p MBB.
bool X86ExpandPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  // MBBI may be invalidated by the expansion.
  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineBasicBlock::iterator NMBBI = std::next(MBBI);
    Modified |= expandMI(MBB, MBBI);
    MBBI = NMBBI;
  }

  return Modified;
}

bool X86ExpandPseudo::expandPseudosWhichAffectControlFlow(MachineFunction &MF) {
  // Currently pseudo which affects control flow is only
  // X86::VASTART_SAVE_XMM_REGS which is located in Entry block.
  // So we do not need to evaluate other blocks.
  for (MachineInstr &Instr : MF.front().instrs()) {
    if (Instr.getOpcode() == X86::VASTART_SAVE_XMM_REGS) {
      expandVastartSaveXmmRegs(&(MF.front()), Instr);
      return true;
    }
  }

  return false;
}

bool X86ExpandPseudo::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<X86Subtarget>();
  TII = STI->getInstrInfo();
  TRI = STI->getRegisterInfo();
  X86FI = MF.getInfo<X86MachineFunctionInfo>();
  X86FL = STI->getFrameLowering();

  bool Modified = expandPseudosWhichAffectControlFlow(MF);

  for (MachineBasicBlock &MBB : MF)
    Modified |= expandMBB(MBB);
  return Modified;
}

/// Returns an instance of the pseudo instruction expansion pass.
FunctionPass *llvm::createX86ExpandPseudoPass() {
  return new X86ExpandPseudo();
}
