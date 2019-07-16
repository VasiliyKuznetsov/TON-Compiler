//===------- TVMStackFixup.cpp - Difference between 2 stack models --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TVMStackFixup.h"
#include "TVMStack.h"

#include <algorithm>

#include "TVMSubtarget.h"
#include "TVMExtras.h"
#include "TVMUtilities.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"

namespace llvm {

StackFixup StackFixup::Diff(const Stack &to, const Stack &from) {
  StackFixup rv;

  Stack curStack(from);

  SmallVector<StackVreg, 16> fromRegs(from.begin(), from.end());
  SmallVector<StackVreg, 16> toRegs(to.begin(), to.end());

  llvm::sort(fromRegs.begin(), fromRegs.end());
  llvm::erase_if(fromRegs, [](const StackVreg &vreg){
    return vreg.VirtReg == TVMFunctionInfo::UnusedReg; });
  llvm::sort(toRegs.begin(), toRegs.end());
  llvm::erase_if(toRegs, [](const StackVreg &vreg){
    return vreg.VirtReg == TVMFunctionInfo::UnusedReg; });

  SmallVector<StackVreg, 16> delVregs;
  std::set_difference(fromRegs.begin(), fromRegs.end(),
                      toRegs.begin(), toRegs.end(),
                      std::back_inserter(delVregs));
  Stack unmaskedTo(to);
  unmaskedTo.fillUnusedRegs(delVregs);
  auto removeElem = [&](const StackVreg &vreg) {
    rv.removeElem(curStack, vreg);
  };
  if (!delVregs.empty()) {
    // Sorting regs-to-delete by position from top of the stack
    llvm::sort(delVregs.begin(), delVregs.end(),
               [&from](const StackVreg &L, const StackVreg &R) {
      return from.position(L) < from.position(R);
    });

    // Generate changes to delete unused vregs
    llvm::for_each(delVregs, removeElem);
  }

  fromRegs = SmallVector<StackVreg, 16>(curStack.begin(), curStack.end());
  toRegs = SmallVector<StackVreg, 16>(unmaskedTo.begin(), unmaskedTo.end());

  llvm::sort(fromRegs.begin(), fromRegs.end());
  llvm::sort(toRegs.begin(), toRegs.end());

  delVregs.clear();
  std::set_difference(fromRegs.begin(), fromRegs.end(),
                      toRegs.begin(), toRegs.end(),
                      std::back_inserter(delVregs));
  llvm::for_each(delVregs, removeElem);

  fromRegs = SmallVector<StackVreg, 16>(curStack.begin(), curStack.end());
  toRegs = SmallVector<StackVreg, 16>(unmaskedTo.begin(), unmaskedTo.end());

  llvm::sort(fromRegs.begin(), fromRegs.end());
  llvm::sort(toRegs.begin(), toRegs.end());

  SmallVector<StackVreg, 16> addVregs;
  std::set_difference(toRegs.begin(), toRegs.end(),
                      fromRegs.begin(), fromRegs.end(),
                      std::back_inserter(addVregs));

  // Generate changes to insert copies (pushes)
  auto addElem = [&](const StackVreg &vreg) {
    if (vreg.VirtReg == TVMFunctionInfo::UnusedReg) {
      rv(curStack += rv(pushUndef()));
      return;
    }
    auto i = curStack.position(vreg);
    rv(curStack += rv(pushI(i)));
  };
  llvm::for_each(addVregs, addElem);

  // Generate changes to re-order
  assert(llvm::size(unmaskedTo) == llvm::size(curStack));
  generateVagonXchgs(rv, curStack, unmaskedTo);
  rv.optimize();
  return rv;
}

// Range of stack elements, corresponding to equal consecutive range
//  in src pattern
// { abcde- }: {ab} = vagon[4:3]; {dc} = vagon[2:1]inv
struct Vagon {
  unsigned DeepIdx; // Deep stack index (first element in range)
  unsigned TopIdx;  // Top stack index (last element in range)
  bool Inverted;    // This range exists in src pattern, but reversed
  Vagon(unsigned DeepIdx, unsigned TopIdx, bool Inverted)
    : DeepIdx(DeepIdx), TopIdx(TopIdx), Inverted(Inverted) {
    assert(DeepIdx >= TopIdx && "Bad Vagon");
  }
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
  void print(raw_ostream &OS) const {
    OS << "[" << DeepIdx << ":" << TopIdx << "]";
    if (Inverted)
      OS << "inv";
  }
  friend raw_ostream& operator<<(raw_ostream &OS, const Vagon &V) {
    V.print(OS);
    return OS;
  }
#endif
};

// Train - splitting of dst stack pattern into subranges equal to subranges
//  in src pattern. Sequence of Vagons.
// Example 0:
// dst: { abcde- }, src: { deabc- } => {[abc] [de]-}
// Train = [2:0][4:3]
// Example 1:
// dst: { abcde- }, src: { edabc- } => {[abc] ~[ed]-}
// Train = [2:0][4:3]inv
struct Train {
  SmallVector<Vagon, 8> Vagons;

  using AquiredVec = SmallVector<bool, 32>;

  // Search specified Vreg in source pattern (not yet aquired register)
  // Returns empty Optional<unsigned>() if unaquired Vreg not found
  static Optional<unsigned>
  searchSrc(const Stack &Src, AquiredVec &Aquired, StackVreg Vreg) {
    unsigned Sz = Src.size();
    for (unsigned RevJ = 0; RevJ < Sz; ++RevJ) {
      unsigned IdxJ = Sz - RevJ - 1;
      if (Src[IdxJ] == Vreg && !Aquired[IdxJ])
        return IdxJ;
    }
    return Optional<unsigned>();
  }
  // Go right (to stack top in Src) from found register while meeting
  //  same and unaquired registers (same in Src and Dst patterns)
  static unsigned
  goRight(const Stack &Src, const Stack &Dst, AquiredVec &Aquired,
          unsigned SrcIdx, unsigned DstIdx) {
    unsigned Len = 0;
    while (DstIdx && SrcIdx && Dst[--DstIdx] == Src[--SrcIdx] &&
           !Aquired[SrcIdx])
      ++Len;
    return Len;
  }
  // Go left (to stack bottom in Src) from found register while meeting
  //  same and unaquired registers (same in Src and Dst patterns)
  static unsigned
  goLeft(const Stack &Src, const Stack &Dst, AquiredVec &Aquired,
         unsigned SrcIdx, unsigned DstIdx) {
    unsigned Sz = Src.size();
    unsigned Len = 0;
    while (DstIdx && (SrcIdx < Sz - 1) &&
           Dst[--DstIdx] == Src[++SrcIdx] && !Aquired[SrcIdx])
      ++Len;
    return Len;
  };
  // Mark Vregs in Src pattern, aquired by specified Vagon (Vreg range)
  static void setAquired(AquiredVec &Aquired, const Vagon &V) {
    std::fill(Aquired.begin() + V.TopIdx, Aquired.begin() + V.DeepIdx + 1,
              true);
  }
  static Train build(const Stack &Src, const Stack &Dst) {
    Train Rv;
    assert(Src.size() == Dst.size() && "Train for different stack sizes");
    unsigned Sz = Src.size();
    SmallVector<bool, 32> Aquired(Sz, false);
    for (unsigned RevI = 0; RevI < Sz; ++RevI) {
      unsigned IdxI = Sz - RevI - 1;
      auto dstVreg = Dst[IdxI];
      auto foundSrc = searchSrc(Src, Aquired, dstVreg);
      assert(foundSrc && "Train build error: Src not found");
      unsigned RightLen = goRight(Src, Dst, Aquired, *foundSrc, IdxI);
      unsigned LeftLen = goLeft(Src, Dst, Aquired, *foundSrc, IdxI);
      if (LeftLen > RightLen) {
        Vagon V(*foundSrc + LeftLen, *foundSrc, true);
        setAquired(Aquired, V);
        Rv.Vagons.push_back(V);
        RevI += LeftLen;
      } else {
        Vagon V(*foundSrc, *foundSrc - RightLen, false);
        setAquired(Aquired, V);
        Rv.Vagons.push_back(V);
        RevI += RightLen;
      }
    }
    return Rv;
  }
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
  void print(raw_ostream &OS) const {
    for (auto V : Vagons)
      V.print(OS);
  }
  friend raw_ostream& operator<<(raw_ostream &OS, const Train &V) {
    V.print(OS);
    return OS;
  }
#endif
};

void StackFixup::generateVagonXchgs(StackFixup &rv, const Stack &from,
                                    const Stack &to) {
  assert(from.size() == to.size());
  Stack curStack(from);
  unsigned Sz = from.size();

  auto Tr = Train::build(curStack, to);
  auto restVagons = llvm::make_range(Tr.Vagons.begin(), Tr.Vagons.end());
  auto V = *restVagons.begin();
  if (V.DeepIdx + 1 == Sz && !V.Inverted) {
    restVagons = drop_begin(restVagons, 1);
  }
  while (llvm::size(restVagons)) {
    auto V = *restVagons.begin();
    unsigned VagonSz = V.DeepIdx + 1 - V.TopIdx;
    if (V.TopIdx) {
      rv(curStack += rv(makeBlkSwap(VagonSz, V.TopIdx)));
      for (auto &V2 : restVagons)
        if (V2.TopIdx < V.TopIdx) {
          V2.TopIdx += VagonSz;
          V2.DeepIdx += VagonSz;
        }
    }
    if (V.Inverted)
      rv(curStack += rv(makeReverse(VagonSz)));

    restVagons = drop_begin(restVagons, 1);
  }
  assert(curStack == to && "Vagon xchgs error");
}

StackFixup StackFixup::DiffForReturn(const Stack &from,
                                     std::optional<unsigned> Preserved) {
  StackFixup rv;
  Stack curStack(from);
  auto numPops = llvm::size(from);
  if (Preserved) {
    --numPops;
    size_t i = from.position(StackVreg(*Preserved));
    size_t j = llvm::size(from) - 1;
    if (i != j) {
      if (i == 0) {
        rv(curStack += rv(xchgTop(j)));
      } else if (j == 0) {
        rv(curStack += rv(xchgTop(i)));
      } else if (i <= XchgLimit && j <= XchgLimit) {
        rv(curStack += rv(xchg(i, j)));
      } else {
        rv(curStack += rv(xchgTop(i)));
        rv(curStack += rv(xchgTop(j)));
        rv(curStack += rv(xchgTop(i)));
      }
    }
  }
  if (numPops)
    rv(curStack += rv(makeBlkdrop(numPops)));
  rv.optimize();
  return rv;
}

StackFixup StackFixup::DiffForArgs(const Stack &From, const MIArgs &Args) {
  StackFixup rv;
  if (!Args.size())
    return rv;
  Stack CurStack(From);

  unsigned TopUnused = 0;
  for (auto Vreg : CurStack) {
    if (Vreg.VirtReg != TVMFunctionInfo::UnusedReg)
      break;
    ++TopUnused;
  }
  if (TopUnused)
    rv(CurStack += rv(makeBlkdrop(TopUnused)));

  const auto &Ar = Args.getArgs();
  struct argInfo {
    argInfo(const MIArgs &Args, unsigned Idx, const Stack &CurStack) {
      const MIArg &Arg = Args.getArgs()[Idx];
      StackVreg vreg(Arg.Vreg);
      PrevArgs = std::count(Args.getArgs().begin(),
                            Args.getArgs().begin() + Idx, Arg);
      if (Arg.IsKilled)
        Push = CurStack.count(vreg) <= PrevArgs;
      else
        Push = CurStack.count(vreg) <= PrevArgs + 1;
    }
    unsigned PrevArgs; // Prev occurrences of this arg (same vreg)
    bool Push; // Need push (not xchg)
  };

  SmallVector<argInfo, 4> ArgInfo;
  for (unsigned i = 0; i < Args.size(); ++i)
    ArgInfo.push_back(argInfo(Args, i, CurStack));

  unsigned Offset = 0;
  for (unsigned i = 0; i < Args.size(); ++i) {
    StackVreg Vreg(Ar[i].Vreg);
    if (ArgInfo[i].Push) {
      auto Pos = CurStack.position(Vreg);
      rv(CurStack += rv(pushI(Pos)));
      ++Offset;
    } else if (auto Pos = CurStack.position(Offset, Vreg)) {
      rv(CurStack += rv(makeRoll(Pos)));
      ++Offset;
    }
  }
  rv.optimize();
  return rv;
}

void StackFixup::apply(Stack &stack) const {
  for (auto p : Changes)
    stack += p.first;
}

void StackFixup::removeElem(Stack &stack, const StackVreg &vreg) {
  auto &rv= (*this);
  auto i = stack.position(vreg);
  if (i == 0) {
    rv(stack += rv(drop()));
  } else if (i == 1) {
    rv(stack += rv(nip()));
  } else {
    rv(stack += rv(makeRoll(i)));
    rv(stack += rv(drop()));
  }
}

void StackFixup::removeNElem(Stack &stack, const StackVreg &vreg, size_t N) {
  for (unsigned i = 0; i < N; ++i)
    removeElem(stack, vreg);
}

void StackFixup::removeAllElem(Stack &stack, const StackVreg &vreg) {
  removeNElem(stack, vreg, stack.count(vreg));
}

StackFixup::Change StackFixup::makeRoll(unsigned deepElem) {
  if (deepElem == 1)
    return xchgTop(deepElem);
  if (blkswap::goodFor(1, deepElem))
    return blkswap(1, deepElem);
  // TODO: Implement roll when new standart (roll/rollx) will be supported
  // return roll(deepElem);
  return blkswap(1, deepElem);
}

StackFixup::Change StackFixup::makeRollRev(unsigned toDeepElem) {
  if (toDeepElem == 1)
    return xchgTop(toDeepElem);
  if (blkswap::goodFor(toDeepElem, 1))
    return blkswap(toDeepElem, 1);
  // TODO: Implement roll when new standart (roll/rollx) will be supported
  // return roll(-static_cast<int>(toDeepElem));
  return blkswap(toDeepElem, 1);
}

StackFixup::Change StackFixup::makeBlkSwap(unsigned deepElems,
                                           unsigned topElems) {
  if (deepElems == 1)
    return makeRoll(topElems);
  if (topElems == 1)
    return makeRollRev(deepElems);
  return blkswap(deepElems, topElems);
}

StackFixup::Change StackFixup::makeReverse(unsigned Sz) {
  assert(Sz > 1);
  if (Sz == 2)
    return xchgTop(1);
  return reverse(Sz, 0);
}

StackFixup::Change StackFixup::makeBlkdrop(unsigned Sz) {
  assert(Sz > 0);
  if (Sz == 1)
    return drop();
  return blkdrop(Sz);
}

void StackFixup::optimizeEqualXchgs() {
  if (Changes.empty())
    return;

  // remove equal consecutive xchgs
  // { <before>, xchgTop i, xchgTop i, <after> } => { <before>, <after> }
  auto it = std::next(Changes.begin());
  while (it != Changes.end()) {
    if (it == Changes.begin()) {
      ++it;
      continue;
    }
    auto prevIt = std::prev(it);
    auto cur = it->first;
    auto prev = prevIt->first;

    if (auto curXchg = std::get_if<xchgTop>(&cur)) {
      if (auto topXchg = std::get_if<xchgTop>(&prev)) {
        if (*curXchg == *topXchg) {
          auto nextIdx = prevIt - Changes.begin();
          Changes.erase(prevIt, std::next(it));
          it = Changes.begin() + nextIdx;
          continue;
        }
      }
    }
    ++it;
  }
}

void StackFixup::optimize() {
  if (Changes.empty())
    return;
  optimizeEqualXchgs();
}

void StackFixup::operator()(const Stack &stack) {
  setLastComment(stack.toString());
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void StackFixup::printElem(raw_ostream &OS, const Change &change) const {
  visit(overloaded{
      [&](drop){ OS << "drop"; },
      [&](nip){ OS << "nip"; },
      [&](xchgTop v){ OS << "xchg s(" << v.i << ")"; },
      [&](xchg v){ OS << "xchg s(" << v.i << "), s(" << v.j << ")"; },
      [&](pushI v){ OS << "push s(" << v.i << ")"; },
      [&](pushUndef){ OS << "zero"; },
      [&](blkswap v){ OS << "blkswap " << v.deepSz << ", " << v.topSz; },
      [&](roll v){
        if (v.i > 0)
          OS << "roll " << v.i;
        else
          OS << "rollrev " << -v.i;
      },
      [&](reverse v){ OS << "reverse " << v.deepSz << ", " << v.topIdx; },
      [&](blkdrop v){ OS << "blkdrop " << v.sz; },
      [&](const doubleChange &v){ OS << "double_" << v.codeName()
                                  << " " << v.args[0] << ", " << v.args[1]; },
      [&](const tripleChange &v){ OS << "triple_" << v.codeName()
                                  << " " << v.args[0] << ", " << v.args[1]
                                  << ", " << v.args[2]; }
    }, change);
}

void StackFixup::print(raw_ostream &OS) const {
  llvm::for_each(Changes, [&](const std::pair<Change, std::string> &v){
    printElem(OS, v.first);
    OS << "; " << v.second << "\n";
  });
}

void StackFixup::dump() const {
  print(dbgs());
}

#endif

MachineInstr *StackFixup::InstructionGenerator::
operator()(const std::pair<Change, std::string> &pair) const {
  MachineInstr *MI = nullptr;
  visit(overloaded{
      [&](drop){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::DROP)).getInstr();
      },
      [&](nip){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::NIP)).getInstr();
      },
      [&](swap){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::SWAP)).getInstr();
      },
      [&](xchgTop v){
        if (v.i <= XchgLimit) {
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::XCHG_TOP)).addImm(v.i)
                       .getInstr();
        } else if (v.i <= XchgDeepLimit) {
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::XCHG_TOP_DEEP))
                       .addImm(v.i)
                       .getInstr();
        }
      },
      [&](xchg v){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::XCHG))
                     .addImm(v.i).addImm(v.j)
                     .getInstr();
      },
      [&](pushI v){
        if (!v.i)
            MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::DUP)).getInstr();
        else
            MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::PUSH)).addImm(v.i)
                         .getInstr();
      },
      [&](pushUndef){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::ZERO)).getInstr();
      },
      [&](blkswap v){
        if (v.isImm()) {
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::BLKSWAP))
                       .addImm(v.deepSz).addImm(v.topSz)
                       .getInstr();
        } else {
          BuildMI(*MBB, InsertPt, DL, TII->get(TVM::CONST_U64_S)).
                  addImm(v.deepSz);
          BuildMI(*MBB, InsertPt, DL, TII->get(TVM::CONST_U64_S)).
                  addImm(v.topSz);
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::BLKSWX)).getInstr();
        }
      },
      [&](roll v){
        if (v.isImmRoll()) {
          auto rollOp = v.i > 0 ? TVM::ROLL : TVM::ROLLREV;
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(rollOp))
                       .addImm(std::abs(v.i))
                       .getInstr();
        } else {
          auto rollOp = v.i > 0 ? TVM::ROLLX : TVM::ROLLREVX;
          BuildMI(*MBB, InsertPt, DL, TII->get(TVM::CONST_U64_S)).
                  addImm(std::abs(v.i));
          MI = BuildMI(*MBB, InsertPt, DL, TII->get(rollOp)).getInstr();
        }
      },
      [&](reverse v){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::REVERSE))
                     .addImm(v.deepSz)
                     .addImm(v.topIdx)
                     .getInstr();
      },
      [&](blkdrop v){
        MI = BuildMI(*MBB, InsertPt, DL, TII->get(TVM::BLKDROP))
                     .addImm(v.sz)
                     .getInstr();
      },
      [&](const doubleChange &v){
        unsigned Opcodes[] = {
          TVM::XCHG2, // XCHG, XCHG
          TVM::XCPU,  // XCHG, PUSH
          TVM::PUXC,  // PUSH, XCHG
          TVM::PUSH2  // PUSH, PUSH
        };

        MI = BuildMI(*MBB, InsertPt, DL, TII->get(Opcodes[v.code()]))
                     .addImm(v.args[0])
                     .addImm(v.args[1])
                     .getInstr();
      },
      [&](const tripleChange &v){
        unsigned Opcodes[] = {
          TVM::XCHG3,  // XCHG, XCHG, XCHG
          TVM::XC2PU,  // XCHG, XCHG, PUSH
          TVM::XCPUXC, // XCHG, PUSH, XCHG
          TVM::XCPU2,  // XCHG, PUSH, PUSH
          TVM::PUXC2,  // PUSH, XCHG, XCHG
          TVM::PUXCPU, // PUSH, XCHG, PUSH
          TVM::PU2XC,  // PUSH, PUSH, XCHG
          TVM::PUSH3   // PUSH, PUSH, PUSH
        };

        MI = BuildMI(*MBB, InsertPt, DL, TII->get(Opcodes[v.code()]))
                     .addImm(v.args[0])
                     .addImm(v.args[1])
                     .addImm(v.args[2])
                     .getInstr();
      }
    }, pair.first);

  assert(MI && "MI is not generated");

  MFI->addStackModelComment(MI, pair.second);
  return MI;
}

void StackFixup::InstructionGenerator::operator()(const StackFixup &v) const {
  llvm::for_each(v.getChanges(), *this);
}

} // namespace llvm
