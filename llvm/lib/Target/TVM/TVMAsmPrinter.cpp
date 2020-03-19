//===-- TVMAsmPrinter.cpp - TVM LLVM assembly writer ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the TVM assembly language.
//
//===----------------------------------------------------------------------===//

#include "InstPrinter/TVMInstPrinter.h"
#include "TVM.h"
#include "TVMInstrInfo.h"
#include "TVMMCInstLower.h"
#include "TVMMCExpr.h"
#include "TVMMachineFunctionInfo.h"
#include "TVMTargetMachine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
class TVMAsmPrinter : public AsmPrinter {
public:
  TVMAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)) {}

  StringRef getPassName() const override { return "TVM Assembly Printer"; }

  void printOperand(const MachineInstr *MI, int OpNum, raw_ostream &O,
                    const char *Modifier = nullptr);
  void EmitInstruction(const MachineInstr *MI) override;
  std::string regToString(const MachineOperand &MO);
  void EmitBasicBlockStart(const MachineBasicBlock &MBB) const override;
  void EmitBasicBlockEnd(const MachineBasicBlock &MBB) override;
  void EmitFunctionBodyEnd() override;

  void EmitFunctionHeader() override;

  /// Print a big LLVM constant int (>64 bit) to the .s file.
  void EmitBigInt(const ConstantInt *CI) override;

  bool runOnMachineFunction(MachineFunction &MF) override;
private:
  TVMFunctionInfo *MFI;
};
} // end of anonymous namespace

std::string TVMAsmPrinter::regToString(const MachineOperand &MO) {
  unsigned RegNo = MO.getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) &&
         "Unlowered physical register encountered during assembly printing");
  assert(!MFI->isVRegStackified(RegNo));
  unsigned TVMReg = MFI->getTVMReg(RegNo);
  assert(TVMReg != TVMFunctionInfo::UnusedReg);
  return 's' + utostr(TVMReg);
}

void TVMAsmPrinter::printOperand(const MachineInstr *MI, int OpNum,
                                 raw_ostream &O, const char *Modifier) {
  const MachineOperand &MO = MI->getOperand(OpNum);
  switch (MO.getType()) {
  default:
    llvm_unreachable("Not implemented yet!");
  case MachineOperand::MO_Register:
    O << regToString(MO);
    return;
  case MachineOperand::MO_Immediate:
    if (!Modifier || strcmp(Modifier, "nohash"))
      O << '#';
    O << MO.getImm();
    return;
  }
}

//===----------------------------------------------------------------------===//
void TVMAsmPrinter::EmitInstruction(const MachineInstr *MI) {
  LLVM_DEBUG(dbgs() << "EmitInstruction: " << *MI);
  if (isVerbose())
    for (auto Comment : MFI->getStackModelComments(MI)) {
      OutStreamer->AddComment(Comment);
    }
  switch (MI->getOpcode()) {
  case TVM::ARGUMENT:
  case TVM::ARGUMENT_SLICE:
  case TVM::ARGUMENT_BUILDER:
  case TVM::ARGUMENT_CELL:
  case TVM::ARGUMENT_TUPLE:
    llvm_unreachable("CG only instruction mustn't reach ASM printer");
    break;
  case TVM::REG_TO_REG_COPY_S:
  case TVM::TO_TUPLE_COPY_S:
  case TVM::TO_SLICE_COPY_S:
  case TVM::TO_BUILDER_COPY_S:
  case TVM::TO_CELL_COPY_S:
  case TVM::FROM_TUPLE_COPY_S:
  case TVM::FROM_SLICE_COPY_S:
  case TVM::FROM_BUILDER_COPY_S:
  case TVM::FROM_CELL_COPY_S:
    break;
  case TVM::RETURN_N_S:
    if (isVerbose()) {
      OutStreamer->AddComment("implicit return");
      OutStreamer->AddBlankLine();
    }
    break;
  default:
    TVMMCInstLower MCInstLowering(OutContext, *this);

    MCInst TmpInst;
    MCInstLowering.lower(MI, TmpInst);
    OutStreamer->GetOS() << (MF->size() < 2 ? "  " : "    ");
    EmitToStreamer(*OutStreamer, TmpInst);
  }
}

void TVMAsmPrinter::EmitBasicBlockStart(const MachineBasicBlock &MBB) const {
  if (MF->size() < 2)
    return;
  OutStreamer->AddComment(MBB.getName());
  OutStreamer->EmitRawText("  PUSHCONT {");
}

void TVMAsmPrinter::EmitBasicBlockEnd(const MachineBasicBlock &MBB) {
  if (MF->size() < 2)
    return;
  OutStreamer->EmitRawText("  }");
}

void TVMAsmPrinter::EmitFunctionHeader() {
  const Function &F = MF->getFunction();
  if (F.hasFnAttribute("tvm_raw_func")) {
    OutStreamer->EmitRawText("\t.internal\t:" + CurrentFnSym->getName());
  } else {
    AsmPrinter::EmitFunctionHeader();
  }
}

void TVMAsmPrinter::EmitFunctionBodyEnd() {
  unsigned Blocks = MF->size();
  if (Blocks < 2)
    return;

  auto *FI = MF->getInfo<TVMFunctionInfo>();
  unsigned Arguments = FI->getParams().size();
  unsigned ReturnValues = FI->getResults().size();

  if (Arguments > 0) {
    if (Blocks <= 16 && Arguments <= 16) {
      OutStreamer->EmitRawText("  BLKSWAP\t" + Twine(Arguments) + ", " +
          Twine(Blocks));
    } else {
      OutStreamer->EmitRawText("  PUSHINT\t" + Twine(Arguments));
      OutStreamer->EmitRawText("  PUSHINT\t" + Twine(Blocks));
      OutStreamer->EmitRawText("  BLKSWX");
    }
  }

  OutStreamer->EmitRawText("  PUSH s" + Twine(Blocks + Arguments - 1));
  OutStreamer->EmitRawText("  EXECUTE");

  if (ReturnValues > 0) {
    if (Blocks <= 16 && ReturnValues <= 16) {
      OutStreamer->EmitRawText("  BLKSWAP\t" + Twine(Blocks) + ", " +
          Twine(ReturnValues));
    } else {
      OutStreamer->EmitRawText("  PUSHINT\t" + Twine(Blocks));
      OutStreamer->EmitRawText("  PUSHINT\t" + Twine(ReturnValues));
      OutStreamer->EmitRawText("  BLKSWX");
    }
  }

  if (Blocks < 16) {
    OutStreamer->EmitRawText("  BLKDROP\t" + Twine(Blocks));
  } else {
    OutStreamer->EmitRawText("  PUSHINT\t" + Twine(Blocks));
    OutStreamer->EmitRawText("  DROPX");
  }
}

/// Print a big LLVM constant int (>64 bit) to the .s file.
void TVMAsmPrinter::EmitBigInt(const ConstantInt *CI) {
  SmallString<80> Str;
  CI->getValue().toString(Str, 16, true, true);
  OutStreamer->EmitRawText(Str);
}

bool TVMAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  MFI = MF.getInfo<TVMFunctionInfo>();
  return AsmPrinter::runOnMachineFunction(MF);
}

// Force static initialization.
extern "C" void LLVMInitializeTVMAsmPrinter() {
  RegisterAsmPrinter<TVMAsmPrinter> X(getTheTVMTarget());
}
