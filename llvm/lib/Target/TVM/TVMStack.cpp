//===----------- TVMStack.cpp - Model of stack used in TVM ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Define classes to manipulate with program model of TVM stack.
//
//===----------------------------------------------------------------------===//

#include "TVMStack.h"

#include <algorithm>

#include "TVMSubtarget.h"
#include "TVMUtilities.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"

namespace llvm {

Stack::Stack(MachineFunction &MF, size_t Size)
    : TRI(MF.getSubtarget<TVMSubtarget>().getRegisterInfo()),
      MRI(&MF.getRegInfo()),
      Data(Size, StackVreg(TVMFunctionInfo::UnusedReg)) {}

Stack Stack::plusArgs(SmallVector<StackVreg, 4> &Args) const {
  Stack rv(*this);
  auto revArgs = reverse(Args);
  rv.Data.insert(rv.Data.begin(), revArgs.begin(), revArgs.end());
  return rv;
}

/// If \par MO is no longer used after \par MI.
static bool isKilled(const MachineInstr &MI, unsigned Register,
                     const LiveIntervals &LIS) {
  const LiveInterval &LI = LIS.getInterval(Register);
  // If there is no live interval starting from the current instruction
  // for the given argument, the argument is killed.
  if (!LI.getVNInfoAt(LIS.getInstructionIndex(MI).getRegSlot()))
    return true;
  for (size_t I = 0, E = MI.getNumDefs(); I < E; ++I)
    if (MI.getOperand(I).isReg() && MI.getOperand(I).getReg() == Register)
      return true;
  return false;
}

Stack Stack::reqArgs(MachineInstr *MI, const LiveIntervals &LIS) const {
  Stack rv(*this);
  SmallVector<StackVreg, 4> Ops;
  auto regUses = llvm::make_filter_range(MI->uses(),
                   [](const MachineOperand &Op) { return Op.isReg(); });
  for (auto Arg : regUses) {
    StackVreg vreg(Arg.getReg());
    auto it = llvm::find(rv.Data, vreg);
    if (it != rv.Data.end()) {
      Ops.push_back(*it);
      if (isKilled(*MI, vreg.VirtReg, LIS))
        rv.Data.erase(it);
    } else {
      auto dupIt = llvm::find(Ops, vreg);
      assert(dupIt != Ops.end() && "No required argument in stack");
      Ops.push_back(*dupIt);
    }
  }
  auto reverted = llvm::reverse(Ops);
  rv.Data.insert(rv.Data.begin(), reverted.begin(), reverted.end());
  return rv;
}

Stack Stack::filteredByLiveIns(MachineBasicBlock &MBB,
                               const LiveIntervals &LIS) const {
  Stack rv(*this);
  for (StackVreg &vreg : rv.Data) {
    if (!LIS.hasInterval(vreg.VirtReg) ||
        !LIS.isLiveInToMBB(LIS.getInterval(vreg.VirtReg), &MBB)) {
      vreg = StackVreg(TVMFunctionInfo::UnusedReg);
    }
  }
  return rv;
}

void Stack::addDef(unsigned Reg, const DILocalVariable *DbgVar) {
  Data.push_front(StackVreg(Reg, DbgVar));
}

Stack& Stack::operator += (const StackFixup::Change &change) {
  std::visit(overloaded{
      [this](StackFixup::drop){
        Data.pop_front();
      },
      [this](StackFixup::nip) {
        Data.erase(std::next(Data.begin()));
      },
      [this](StackFixup::swap) {
        std::swap(Data[0], Data[1]);
      },
      [this](StackFixup::xchgTop v) {
        std::swap(Data[0], Data[v.i]);
      },
      [this](StackFixup::xchg v) {
        std::swap(Data[v.i], Data[v.j]);
      },
      [this](StackFixup::dup){ Data.push_front(Data.front()); },
      [this](StackFixup::pushI v){ Data.push_front(Data[v.i]); }
    }, change);
  return *this;
}

void Stack::print(raw_ostream &OS) const {
  OS << "{ ";
  if (!Data.empty()) {
    llvm::for_each(reverse(Data), [&OS, this](const StackVreg &Elem) {
      printElement(OS, Elem);
      OS << " | ";
    });
  }
  OS << "- }";
}

std::string Stack::toString() const {
  std::string buf;
  raw_string_ostream os(buf);
  print(os);
  os.flush();
  return buf;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
/// Allow easy printing of the stack from the debugger.
void Stack::dump() const {
  print(dbgs());
  dbgs() << "\n";
  dbgs().flush();
}
#endif

void Stack::printElement(raw_ostream &OS, const StackVreg &Vreg) const {
  if (Vreg.VirtReg == TVMFunctionInfo::UnusedReg) {
    OS << "x";
    return;
  }
  OS << printReg(Vreg.VirtReg, TRI, 0, MRI);
  if (Vreg.DbgVar) {
    OS << "(" << Vreg.DbgVar->getName() << ")";
  }
}

StackFixup Stack::operator-(const Stack &v) const {
  return StackFixup::Diff(*this, v);
}

} // namespace llvm
