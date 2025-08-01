//===- llvm/CodeGen/VirtRegMap.cpp - Virtual Register Map -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the VirtRegMap class.
//
// It also contains implementations of the Spiller interface, which, given a
// virtual register map and a machine function, eliminates all virtual
// references by replacing them with physical register references - adding spill
// code as necessary.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/MC/LaneBitmask.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <iterator>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

STATISTIC(NumSpillSlots, "Number of spill slots allocated");
STATISTIC(NumIdCopies,   "Number of identity moves eliminated after rewriting");

//===----------------------------------------------------------------------===//
//  VirtRegMap implementation
//===----------------------------------------------------------------------===//

char VirtRegMapWrapperLegacy::ID = 0;

INITIALIZE_PASS(VirtRegMapWrapperLegacy, "virtregmap", "Virtual Register Map",
                false, true)

void VirtRegMap::init(MachineFunction &mf) {
  MRI = &mf.getRegInfo();
  TII = mf.getSubtarget().getInstrInfo();
  TRI = mf.getSubtarget().getRegisterInfo();
  MF = &mf;

  Virt2PhysMap.clear();
  Virt2StackSlotMap.clear();
  Virt2SplitMap.clear();
  Virt2ShapeMap.clear();

  grow();
}

void VirtRegMap::grow() {
  unsigned NumRegs = MF->getRegInfo().getNumVirtRegs();
  Virt2PhysMap.resize(NumRegs);
  Virt2StackSlotMap.resize(NumRegs);
  Virt2SplitMap.resize(NumRegs);
}

void VirtRegMap::assignVirt2Phys(Register virtReg, MCRegister physReg) {
  assert(virtReg.isVirtual() && physReg.isPhysical());
  assert(!Virt2PhysMap[virtReg] &&
         "attempt to assign physical register to already mapped "
         "virtual register");
  assert(!getRegInfo().isReserved(physReg) &&
         "Attempt to map virtReg to a reserved physReg");
  Virt2PhysMap[virtReg] = physReg;
}

unsigned VirtRegMap::createSpillSlot(const TargetRegisterClass *RC) {
  unsigned Size = TRI->getSpillSize(*RC);
  Align Alignment = TRI->getSpillAlign(*RC);
  // Set preferred alignment if we are still able to realign the stack
  auto &ST = MF->getSubtarget();
  Align CurrentAlign = ST.getFrameLowering()->getStackAlign();
  if (Alignment > CurrentAlign && !ST.getRegisterInfo()->canRealignStack(*MF)) {
    Alignment = CurrentAlign;
  }
  int SS = MF->getFrameInfo().CreateSpillStackObject(Size, Alignment);
  ++NumSpillSlots;
  return SS;
}

bool VirtRegMap::hasPreferredPhys(Register VirtReg) const {
  Register Hint = MRI->getSimpleHint(VirtReg);
  if (!Hint.isValid())
    return false;
  if (Hint.isVirtual())
    Hint = getPhys(Hint);
  return Register(getPhys(VirtReg)) == Hint;
}

bool VirtRegMap::hasKnownPreference(Register VirtReg) const {
  std::pair<unsigned, Register> Hint = MRI->getRegAllocationHint(VirtReg);
  if (Hint.second.isPhysical())
    return true;
  if (Hint.second.isVirtual())
    return hasPhys(Hint.second);
  return false;
}

int VirtRegMap::assignVirt2StackSlot(Register virtReg) {
  assert(virtReg.isVirtual());
  assert(Virt2StackSlotMap[virtReg] == NO_STACK_SLOT &&
         "attempt to assign stack slot to already spilled register");
  const TargetRegisterClass* RC = MF->getRegInfo().getRegClass(virtReg);
  return Virt2StackSlotMap[virtReg] = createSpillSlot(RC);
}

void VirtRegMap::assignVirt2StackSlot(Register virtReg, int SS) {
  assert(virtReg.isVirtual());
  assert(Virt2StackSlotMap[virtReg] == NO_STACK_SLOT &&
         "attempt to assign stack slot to already spilled register");
  assert((SS >= 0 ||
          (SS >= MF->getFrameInfo().getObjectIndexBegin())) &&
         "illegal fixed frame index");
  Virt2StackSlotMap[virtReg] = SS;
}

void VirtRegMap::print(raw_ostream &OS, const Module*) const {
  OS << "********** REGISTER MAP **********\n";
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (Virt2PhysMap[Reg]) {
      OS << '[' << printReg(Reg, TRI) << " -> "
         << printReg(Virt2PhysMap[Reg], TRI) << "] "
         << TRI->getRegClassName(MRI->getRegClass(Reg)) << "\n";
    }
  }

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (Virt2StackSlotMap[Reg] != VirtRegMap::NO_STACK_SLOT) {
      OS << '[' << printReg(Reg, TRI) << " -> fi#" << Virt2StackSlotMap[Reg]
         << "] " << TRI->getRegClassName(MRI->getRegClass(Reg)) << "\n";
    }
  }
  OS << '\n';
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void VirtRegMap::dump() const {
  print(dbgs());
}
#endif

AnalysisKey VirtRegMapAnalysis::Key;

PreservedAnalyses
VirtRegMapPrinterPass::run(MachineFunction &MF,
                           MachineFunctionAnalysisManager &MFAM) {
  OS << MFAM.getResult<VirtRegMapAnalysis>(MF);
  return PreservedAnalyses::all();
}

VirtRegMap VirtRegMapAnalysis::run(MachineFunction &MF,
                                   MachineFunctionAnalysisManager &MAM) {
  VirtRegMap VRM;
  VRM.init(MF);
  return VRM;
}

//===----------------------------------------------------------------------===//
//                              VirtRegRewriter
//===----------------------------------------------------------------------===//
//
// The VirtRegRewriter is the last of the register allocator passes.
// It rewrites virtual registers to physical registers as specified in the
// VirtRegMap analysis. It also updates live-in information on basic blocks
// according to LiveIntervals.
//
namespace {

class VirtRegRewriter {
  MachineFunction *MF = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  SlotIndexes *Indexes = nullptr;
  LiveIntervals *LIS = nullptr;
  LiveRegMatrix *LRM = nullptr;
  VirtRegMap *VRM = nullptr;
  LiveDebugVariables *DebugVars = nullptr;
  DenseSet<Register> RewriteRegs;
  bool ClearVirtRegs;

  void rewrite();
  void addMBBLiveIns();
  bool readsUndefSubreg(const MachineOperand &MO) const;
  void addLiveInsForSubRanges(const LiveInterval &LI, MCRegister PhysReg) const;
  void handleIdentityCopy(MachineInstr &MI);
  void expandCopyBundle(MachineInstr &MI) const;
  bool subRegLiveThrough(const MachineInstr &MI, MCRegister SuperPhysReg) const;
  LaneBitmask liveOutUndefPhiLanesForUndefSubregDef(
      const LiveInterval &LI, const MachineBasicBlock &MBB, unsigned SubReg,
      MCRegister PhysReg, const MachineInstr &MI) const;

public:
  VirtRegRewriter(bool ClearVirtRegs, SlotIndexes *Indexes, LiveIntervals *LIS,
                  LiveRegMatrix *LRM, VirtRegMap *VRM,
                  LiveDebugVariables *DebugVars)
      : Indexes(Indexes), LIS(LIS), LRM(LRM), VRM(VRM), DebugVars(DebugVars),
        ClearVirtRegs(ClearVirtRegs) {}

  bool run(MachineFunction &);
};

class VirtRegRewriterLegacy : public MachineFunctionPass {
public:
  static char ID;
  bool ClearVirtRegs;
  VirtRegRewriterLegacy(bool ClearVirtRegs = true)
      : MachineFunctionPass(ID), ClearVirtRegs(ClearVirtRegs) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction&) override;

  MachineFunctionProperties getSetProperties() const override {
    if (ClearVirtRegs) {
      return MachineFunctionProperties().setNoVRegs();
    }

    return MachineFunctionProperties();
  }
};

} // end anonymous namespace

char VirtRegRewriterLegacy::ID = 0;

char &llvm::VirtRegRewriterID = VirtRegRewriterLegacy::ID;

INITIALIZE_PASS_BEGIN(VirtRegRewriterLegacy, "virtregrewriter",
                      "Virtual Register Rewriter", false, false)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_END(VirtRegRewriterLegacy, "virtregrewriter",
                    "Virtual Register Rewriter", false, false)

void VirtRegRewriterLegacy::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addRequired<SlotIndexesWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();

  if (!ClearVirtRegs)
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();

  MachineFunctionPass::getAnalysisUsage(AU);
}

bool VirtRegRewriterLegacy::runOnMachineFunction(MachineFunction &MF) {
  VirtRegMap &VRM = getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  LiveRegMatrix &LRM = getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  SlotIndexes &Indexes = getAnalysis<SlotIndexesWrapperPass>().getSI();
  LiveDebugVariables &DebugVars =
      getAnalysis<LiveDebugVariablesWrapperLegacy>().getLDV();

  VirtRegRewriter R(ClearVirtRegs, &Indexes, &LIS, &LRM, &VRM, &DebugVars);
  return R.run(MF);
}

PreservedAnalyses
VirtRegRewriterPass::run(MachineFunction &MF,
                         MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);

  VirtRegMap &VRM = MFAM.getResult<VirtRegMapAnalysis>(MF);
  LiveIntervals &LIS = MFAM.getResult<LiveIntervalsAnalysis>(MF);
  LiveRegMatrix &LRM = MFAM.getResult<LiveRegMatrixAnalysis>(MF);
  SlotIndexes &Indexes = MFAM.getResult<SlotIndexesAnalysis>(MF);
  LiveDebugVariables &DebugVars =
      MFAM.getResult<LiveDebugVariablesAnalysis>(MF);

  VirtRegRewriter R(ClearVirtRegs, &Indexes, &LIS, &LRM, &VRM, &DebugVars);
  if (!R.run(MF))
    return PreservedAnalyses::all();

  auto PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserve<SlotIndexesAnalysis>();
  PA.preserve<LiveStacksAnalysis>();
  // LiveDebugVariables is preserved by default, so clear it
  // if this VRegRewriter is the last one in the pipeline.
  if (ClearVirtRegs)
    PA.abandon<LiveDebugVariablesAnalysis>();
  return PA;
}

bool VirtRegRewriter::run(MachineFunction &fn) {
  MF = &fn;
  TRI = MF->getSubtarget().getRegisterInfo();
  TII = MF->getSubtarget().getInstrInfo();
  MRI = &MF->getRegInfo();

  LLVM_DEBUG(dbgs() << "********** REWRITE VIRTUAL REGISTERS **********\n"
                    << "********** Function: " << MF->getName() << '\n');
  LLVM_DEBUG(VRM->dump());

  // Add kill flags while we still have virtual registers.
  LIS->addKillFlags(VRM);

  // Live-in lists on basic blocks are required for physregs.
  addMBBLiveIns();

  // Rewrite virtual registers.
  rewrite();

  if (ClearVirtRegs) {
    // Write out new DBG_VALUE instructions.

    // We only do this if ClearVirtRegs is specified since this should be the
    // final run of the pass and we don't want to emit them multiple times.
    DebugVars->emitDebugValues(VRM);

    // All machine operands and other references to virtual registers have been
    // replaced. Remove the virtual registers and release all the transient data.
    VRM->clearAllVirt();
    MRI->clearVirtRegs();
  }

  return true;
}

void VirtRegRewriter::addLiveInsForSubRanges(const LiveInterval &LI,
                                             MCRegister PhysReg) const {
  assert(!LI.empty());
  assert(LI.hasSubRanges());

  using SubRangeIteratorPair =
      std::pair<const LiveInterval::SubRange *, LiveInterval::const_iterator>;

  SmallVector<SubRangeIteratorPair, 4> SubRanges;
  SlotIndex First;
  SlotIndex Last;
  for (const LiveInterval::SubRange &SR : LI.subranges()) {
    SubRanges.push_back(std::make_pair(&SR, SR.begin()));
    if (!First.isValid() || SR.segments.front().start < First)
      First = SR.segments.front().start;
    if (!Last.isValid() || SR.segments.back().end > Last)
      Last = SR.segments.back().end;
  }

  // Check all mbb start positions between First and Last while
  // simultaneously advancing an iterator for each subrange.
  for (SlotIndexes::MBBIndexIterator MBBI = Indexes->getMBBLowerBound(First);
       MBBI != Indexes->MBBIndexEnd() && MBBI->first <= Last; ++MBBI) {
    SlotIndex MBBBegin = MBBI->first;
    // Advance all subrange iterators so that their end position is just
    // behind MBBBegin (or the iterator is at the end).
    LaneBitmask LaneMask;
    for (auto &RangeIterPair : SubRanges) {
      const LiveInterval::SubRange *SR = RangeIterPair.first;
      LiveInterval::const_iterator &SRI = RangeIterPair.second;
      while (SRI != SR->end() && SRI->end <= MBBBegin)
        ++SRI;
      if (SRI == SR->end())
        continue;
      if (SRI->start <= MBBBegin)
        LaneMask |= SR->LaneMask;
    }
    if (LaneMask.none())
      continue;
    MachineBasicBlock *MBB = MBBI->second;
    MBB->addLiveIn(PhysReg, LaneMask);
  }
}

// Compute MBB live-in lists from virtual register live ranges and their
// assignments.
void VirtRegRewriter::addMBBLiveIns() {
  for (unsigned Idx = 0, IdxE = MRI->getNumVirtRegs(); Idx != IdxE; ++Idx) {
    Register VirtReg = Register::index2VirtReg(Idx);
    if (MRI->reg_nodbg_empty(VirtReg))
      continue;
    LiveInterval &LI = LIS->getInterval(VirtReg);
    if (LI.empty() || LIS->intervalIsInOneMBB(LI))
      continue;
    // This is a virtual register that is live across basic blocks. Its
    // assigned PhysReg must be marked as live-in to those blocks.
    MCRegister PhysReg = VRM->getPhys(VirtReg);
    if (!PhysReg) {
      // There may be no physical register assigned if only some register
      // classes were already allocated.
      assert(!ClearVirtRegs && "Unmapped virtual register");
      continue;
    }

    if (LI.hasSubRanges()) {
      addLiveInsForSubRanges(LI, PhysReg);
    } else {
      // Go over MBB begin positions and see if we have segments covering them.
      // The following works because segments and the MBBIndex list are both
      // sorted by slot indexes.
      SlotIndexes::MBBIndexIterator I = Indexes->MBBIndexBegin();
      for (const auto &Seg : LI) {
        I = Indexes->getMBBLowerBound(I, Seg.start);
        for (; I != Indexes->MBBIndexEnd() && I->first < Seg.end; ++I) {
          MachineBasicBlock *MBB = I->second;
          MBB->addLiveIn(PhysReg);
        }
      }
    }
  }

  // Sort and unique MBB LiveIns as we've not checked if SubReg/PhysReg were in
  // each MBB's LiveIns set before calling addLiveIn on them.
  for (MachineBasicBlock &MBB : *MF)
    MBB.sortUniqueLiveIns();
}

/// Returns true if the given machine operand \p MO only reads undefined lanes.
/// The function only works for use operands with a subregister set.
bool VirtRegRewriter::readsUndefSubreg(const MachineOperand &MO) const {
  // Shortcut if the operand is already marked undef.
  if (MO.isUndef())
    return true;

  Register Reg = MO.getReg();
  const LiveInterval &LI = LIS->getInterval(Reg);
  const MachineInstr &MI = *MO.getParent();
  SlotIndex BaseIndex = LIS->getInstructionIndex(MI);
  // This code is only meant to handle reading undefined subregisters which
  // we couldn't properly detect before.
  assert(LI.liveAt(BaseIndex) &&
         "Reads of completely dead register should be marked undef already");
  unsigned SubRegIdx = MO.getSubReg();
  assert(SubRegIdx != 0 && LI.hasSubRanges());
  LaneBitmask UseMask = TRI->getSubRegIndexLaneMask(SubRegIdx);
  // See if any of the relevant subregister liveranges is defined at this point.
  for (const LiveInterval::SubRange &SR : LI.subranges()) {
    if ((SR.LaneMask & UseMask).any() && SR.liveAt(BaseIndex))
      return false;
  }
  return true;
}

void VirtRegRewriter::handleIdentityCopy(MachineInstr &MI) {
  if (!MI.isIdentityCopy())
    return;
  LLVM_DEBUG(dbgs() << "Identity copy: " << MI);
  ++NumIdCopies;

  Register DstReg = MI.getOperand(0).getReg();

  // We may have deferred allocation of the virtual register, and the rewrite
  // regs code doesn't handle the liveness update.
  if (DstReg.isVirtual())
    return;

  RewriteRegs.insert(DstReg);

  // Copies like:
  //    %r0 = COPY undef %r0
  //    %al = COPY %al, implicit-def %eax
  // give us additional liveness information: The target (super-)register
  // must not be valid before this point. Replace the COPY with a KILL
  // instruction to maintain this information.
  if (MI.getOperand(1).isUndef() || MI.getNumOperands() > 2) {
    MI.setDesc(TII->get(TargetOpcode::KILL));
    LLVM_DEBUG(dbgs() << "  replace by: " << MI);
    return;
  }

  if (Indexes)
    Indexes->removeSingleMachineInstrFromMaps(MI);
  MI.eraseFromBundle();
  LLVM_DEBUG(dbgs() << "  deleted.\n");
}

/// The liverange splitting logic sometimes produces bundles of copies when
/// subregisters are involved. Expand these into a sequence of copy instructions
/// after processing the last in the bundle. Does not update LiveIntervals
/// which we shouldn't need for this instruction anymore.
void VirtRegRewriter::expandCopyBundle(MachineInstr &MI) const {
  if (!MI.isCopy() && !MI.isKill())
    return;

  if (MI.isBundledWithPred() && !MI.isBundledWithSucc()) {
    SmallVector<MachineInstr *, 2> MIs({&MI});

    // Only do this when the complete bundle is made out of COPYs and KILLs.
    MachineBasicBlock &MBB = *MI.getParent();
    for (MachineBasicBlock::reverse_instr_iterator I =
         std::next(MI.getReverseIterator()), E = MBB.instr_rend();
         I != E && I->isBundledWithSucc(); ++I) {
      if (!I->isCopy() && !I->isKill())
        return;
      MIs.push_back(&*I);
    }
    MachineInstr *FirstMI = MIs.back();

    auto anyRegsAlias = [](const MachineInstr *Dst,
                           ArrayRef<MachineInstr *> Srcs,
                           const TargetRegisterInfo *TRI) {
      for (const MachineInstr *Src : Srcs)
        if (Src != Dst)
          if (TRI->regsOverlap(Dst->getOperand(0).getReg(),
                               Src->getOperand(1).getReg()))
            return true;
      return false;
    };

    // If any of the destination registers in the bundle of copies alias any of
    // the source registers, try to schedule the instructions to avoid any
    // clobbering.
    for (int E = MIs.size(), PrevE = E; E > 1; PrevE = E) {
      for (int I = E; I--; )
        if (!anyRegsAlias(MIs[I], ArrayRef(MIs).take_front(E), TRI)) {
          if (I + 1 != E)
            std::swap(MIs[I], MIs[E - 1]);
          --E;
        }
      if (PrevE == E) {
        MF->getFunction().getContext().emitError(
            "register rewriting failed: cycle in copy bundle");
        break;
      }
    }

    MachineInstr *BundleStart = FirstMI;
    for (MachineInstr *BundledMI : llvm::reverse(MIs)) {
      // If instruction is in the middle of the bundle, move it before the
      // bundle starts, otherwise, just unbundle it. When we get to the last
      // instruction, the bundle will have been completely undone.
      if (BundledMI != BundleStart) {
        BundledMI->removeFromBundle();
        MBB.insert(BundleStart, BundledMI);
      } else if (BundledMI->isBundledWithSucc()) {
        BundledMI->unbundleFromSucc();
        BundleStart = &*std::next(BundledMI->getIterator());
      }

      if (Indexes && BundledMI != FirstMI)
        Indexes->insertMachineInstrInMaps(*BundledMI);
    }
  }
}

/// Check whether (part of) \p SuperPhysReg is live through \p MI.
/// \pre \p MI defines a subregister of a virtual register that
/// has been assigned to \p SuperPhysReg.
bool VirtRegRewriter::subRegLiveThrough(const MachineInstr &MI,
                                        MCRegister SuperPhysReg) const {
  SlotIndex MIIndex = LIS->getInstructionIndex(MI);
  SlotIndex BeforeMIUses = MIIndex.getBaseIndex();
  SlotIndex AfterMIDefs = MIIndex.getBoundaryIndex();
  for (MCRegUnit Unit : TRI->regunits(SuperPhysReg)) {
    const LiveRange &UnitRange = LIS->getRegUnit(Unit);
    // If the regunit is live both before and after MI,
    // we assume it is live through.
    // Generally speaking, this is not true, because something like
    // "RU = op RU" would match that description.
    // However, we know that we are trying to assess whether
    // a def of a virtual reg, vreg, is live at the same time of RU.
    // If we are in the "RU = op RU" situation, that means that vreg
    // is defined at the same time as RU (i.e., "vreg, RU = op RU").
    // Thus, vreg and RU interferes and vreg cannot be assigned to
    // SuperPhysReg. Therefore, this situation cannot happen.
    if (UnitRange.liveAt(AfterMIDefs) && UnitRange.liveAt(BeforeMIUses))
      return true;
  }
  return false;
}

/// Compute a lanemask for undef lanes which need to be preserved out of the
/// defining block for a register assignment for a subregister def. \p PhysReg
/// is assigned to \p LI, which is the main range.
LaneBitmask VirtRegRewriter::liveOutUndefPhiLanesForUndefSubregDef(
    const LiveInterval &LI, const MachineBasicBlock &MBB, unsigned SubReg,
    MCRegister PhysReg, const MachineInstr &MI) const {
  LaneBitmask UndefMask = ~TRI->getSubRegIndexLaneMask(SubReg);
  LaneBitmask LiveOutUndefLanes;

  for (const LiveInterval::SubRange &SR : LI.subranges()) {
    // Figure out which lanes are undef live into a successor.
    LaneBitmask NeedImpDefLanes = UndefMask & SR.LaneMask;
    if (NeedImpDefLanes.any() && !LIS->isLiveOutOfMBB(SR, &MBB)) {
      for (const MachineBasicBlock *Succ : MBB.successors()) {
        if (LIS->isLiveInToMBB(SR, Succ))
          LiveOutUndefLanes |= NeedImpDefLanes;
      }
    }
  }

  SlotIndex MIIndex = LIS->getInstructionIndex(MI);
  SlotIndex BeforeMIUses = MIIndex.getBaseIndex();
  LaneBitmask InterferingLanes =
      LRM->checkInterferenceLanes(BeforeMIUses, MIIndex.getRegSlot(), PhysReg);
  LiveOutUndefLanes &= ~InterferingLanes;

  LLVM_DEBUG(if (LiveOutUndefLanes.any()) {
    dbgs() << "Need live out undef defs for " << printReg(PhysReg)
           << LiveOutUndefLanes << " from " << printMBBReference(MBB) << '\n';
  });

  return LiveOutUndefLanes;
}

void VirtRegRewriter::rewrite() {
  bool NoSubRegLiveness = !MRI->subRegLivenessEnabled();
  SmallVector<Register, 8> SuperDeads;
  SmallVector<Register, 8> SuperDefs;
  SmallVector<Register, 8> SuperKills;

  for (MachineFunction::iterator MBBI = MF->begin(), MBBE = MF->end();
       MBBI != MBBE; ++MBBI) {
    LLVM_DEBUG(MBBI->print(dbgs(), Indexes));
    for (MachineInstr &MI : llvm::make_early_inc_range(MBBI->instrs())) {
      for (MachineOperand &MO : MI.operands()) {
        // Make sure MRI knows about registers clobbered by regmasks.
        if (MO.isRegMask())
          MRI->addPhysRegsUsedFromRegMask(MO.getRegMask());

        if (!MO.isReg() || !MO.getReg().isVirtual())
          continue;
        Register VirtReg = MO.getReg();
        MCRegister PhysReg = VRM->getPhys(VirtReg);
        if (!PhysReg)
          continue;

        assert(Register(PhysReg).isPhysical());

        RewriteRegs.insert(PhysReg);
        assert(!MRI->isReserved(PhysReg) && "Reserved register assignment");

        // Preserve semantics of sub-register operands.
        unsigned SubReg = MO.getSubReg();
        if (SubReg != 0) {
          if (NoSubRegLiveness || !MRI->shouldTrackSubRegLiveness(VirtReg)) {
            // A virtual register kill refers to the whole register, so we may
            // have to add implicit killed operands for the super-register.  A
            // partial redef always kills and redefines the super-register.
            if ((MO.readsReg() && (MO.isDef() || MO.isKill())) ||
                (MO.isDef() && subRegLiveThrough(MI, PhysReg)))
              SuperKills.push_back(PhysReg);

            if (MO.isDef()) {
              // Also add implicit defs for the super-register.
              if (MO.isDead())
                SuperDeads.push_back(PhysReg);
              else
                SuperDefs.push_back(PhysReg);
            }
          } else {
            if (MO.isUse()) {
              if (readsUndefSubreg(MO))
                // We need to add an <undef> flag if the subregister is
                // completely undefined (and we are not adding super-register
                // defs).
                MO.setIsUndef(true);
            } else if (!MO.isDead()) {
              assert(MO.isDef());
              if (MO.isUndef()) {
                const LiveInterval &LI = LIS->getInterval(VirtReg);

                LaneBitmask LiveOutUndefLanes =
                    liveOutUndefPhiLanesForUndefSubregDef(LI, *MBBI, SubReg,
                                                          PhysReg, MI);
                if (LiveOutUndefLanes.any()) {
                  SmallVector<unsigned, 16> CoveringIndexes;

                  // TODO: Just use one super register def if none of the lanes
                  // are needed?
                  if (!TRI->getCoveringSubRegIndexes(MRI->getRegClass(VirtReg),
                                                     LiveOutUndefLanes,
                                                     CoveringIndexes))
                    llvm_unreachable(
                        "cannot represent required subregister defs");

                  // Try to represent the minimum needed live out def as a
                  // sequence of subregister defs.
                  //
                  // FIXME: It would be better if we could directly represent
                  // liveness with a lanemask instead of spamming operands.
                  for (unsigned SubIdx : CoveringIndexes)
                    SuperDefs.push_back(TRI->getSubReg(PhysReg, SubIdx));
                }
              }
            }
          }

          // The def undef and def internal flags only make sense for
          // sub-register defs, and we are substituting a full physreg.  An
          // implicit killed operand from the SuperKills list will represent the
          // partial read of the super-register.
          if (MO.isDef()) {
            MO.setIsUndef(false);
            MO.setIsInternalRead(false);
          }

          // PhysReg operands cannot have subregister indexes.
          PhysReg = TRI->getSubReg(PhysReg, SubReg);
          assert(PhysReg.isValid() && "Invalid SubReg for physical register");
          MO.setSubReg(0);
        }
        // Rewrite. Note we could have used MachineOperand::substPhysReg(), but
        // we need the inlining here.
        MO.setReg(PhysReg);
        MO.setIsRenamable(true);
      }

      // Add any missing super-register kills after rewriting the whole
      // instruction.
      while (!SuperKills.empty())
        MI.addRegisterKilled(SuperKills.pop_back_val(), TRI, true);

      while (!SuperDeads.empty())
        MI.addRegisterDead(SuperDeads.pop_back_val(), TRI, true);

      while (!SuperDefs.empty())
        MI.addRegisterDefined(SuperDefs.pop_back_val(), TRI);

      LLVM_DEBUG(dbgs() << "> " << MI);

      expandCopyBundle(MI);

      // We can remove identity copies right now.
      handleIdentityCopy(MI);
    }
  }

  if (LIS) {
    // Don't bother maintaining accurate LiveIntervals for registers which were
    // already allocated.
    for (Register PhysReg : RewriteRegs) {
      for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
        LIS->removeRegUnit(Unit);
      }
    }
  }

  RewriteRegs.clear();
}

void VirtRegRewriterPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)>) const {
  OS << "virt-reg-rewriter";
  if (!ClearVirtRegs)
    OS << "<no-clear-vregs>";
}

FunctionPass *llvm::createVirtRegRewriter(bool ClearVirtRegs) {
  return new VirtRegRewriterLegacy(ClearVirtRegs);
}
