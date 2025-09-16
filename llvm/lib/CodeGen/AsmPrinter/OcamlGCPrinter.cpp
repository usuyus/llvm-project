//===- OcamlGCPrinter.cpp - Ocaml frametable emitter ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements printing the assembly code for an Ocaml frametable.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/GCMetadataPrinter.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/IR/BuiltinGCs.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace llvm;

namespace {

class OcamlGCMetadataPrinter : public GCMetadataPrinter {
public:
  void beginAssembly(Module &M, GCModuleInfo &Info, AsmPrinter &AP) override;
  void finishAssembly(Module &M, GCModuleInfo &Info, AsmPrinter &AP) override;
  bool emitStackMaps(Module &M, StackMaps &SM, AsmPrinter &AP) override;
};

} // end anonymous namespace

static GCMetadataPrinterRegistry::Add<OcamlGCMetadataPrinter>
    Y("ocaml", "ocaml frametable printer");

void llvm::linkOcamlGCPrinter() {}

static std::string camlGlobalSymName(const Module &M, const char *Id) {
  const std::string &MId = M.getModuleIdentifier();

  std::string SymName;
  SymName += "caml";
  size_t Letter = SymName.size();
  SymName.append(MId.begin(), llvm::find(MId, '.'));
  SymName += "__";
  SymName += Id;

  // Capitalize the first letter of the module name.
  SymName[Letter] = toupper(SymName[Letter]);

  return SymName;
}

static void emitCamlGlobal(const Module &M, MCStreamer &OS, const char *Id) {
  std::string SymName = camlGlobalSymName(M, Id);

  SmallString<128> TmpStr;
  Mangler::getNameWithPrefix(TmpStr, SymName, M.getDataLayout());

  MCSymbol *Sym = OS.getContext().getOrCreateSymbol(TmpStr);

  OS.emitSymbolAttribute(Sym, MCSA_Global);
  OS.emitLabel(Sym);
}

void OcamlGCMetadataPrinter::beginAssembly(Module &M, GCModuleInfo &Info,
                                           AsmPrinter &AP) {
  AP.OutStreamer->switchSection(AP.getObjFileLowering().getTextSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "code_begin");

  AP.OutStreamer->switchSection(AP.getObjFileLowering().getDataSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "data_begin");
}


void OcamlGCMetadataPrinter::finishAssembly(Module &M, GCModuleInfo &Info,
                                         AsmPrinter &AP) {
  AP.OutStreamer->switchSection(AP.getObjFileLowering().getTextSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "code_end");

  AP.OutStreamer->switchSection(AP.getObjFileLowering().getDataSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "data_end");
}

/// Map LLVM DWARF register numbers to OCaml register map.
/// * See llvm/lib/Target/X86/X86RegisterInfo.td for DWARF register numbers.
/// * See Reg_class.gpr_dwarf_reg_numbers for the OCaml register map.
/// CR yusumez: This is target-specific and should probably live in a
/// target-specific location.

// Directly taken from [Reg_class.gpr_dwarf_reg_numbers]:
// https://github.com/oxcaml/oxcaml/blob/main/backend/amd64/reg_class.ml#L26
// Note that R14 and R15 are added for completeness
static constexpr std::array<unsigned, 16> GPR_OCamlToDwarf =
  { 0, 3, 5, 4, 1, 2, 8, 9, 12, 13, 10, 11, 6, 14, 15 };

static constexpr auto GPR_DwarfToOcaml = []() {
  std::array<unsigned, 16> result{};
  for (size_t ocaml_idx = 0; ocaml_idx < GPR_OCamlToDwarf.size(); ++ocaml_idx) {
    unsigned dwarf_reg = GPR_OCamlToDwarf[ocaml_idx];
    if (dwarf_reg < result.size()) {
      result[dwarf_reg] = ocaml_idx;
    }
  }
  return result;
}();

static const unsigned XMMBeginOcaml = 100;
static const unsigned XMMBeginDwarf = 17;
static const unsigned XMMEndDwarf = 32;

static unsigned mapLLVMDwarfRegToOCamlIndex(unsigned DwarfRegNum) {
  if (DwarfRegNum < GPR_DwarfToOcaml.size()) {
    return GPR_DwarfToOcaml[DwarfRegNum];
  } else if (XMMBeginDwarf <= DwarfRegNum && DwarfRegNum <= XMMEndDwarf) {
    return DwarfRegNum - XMMBeginDwarf + XMMBeginOcaml;
  } else {
    report_fatal_error("Unrecognised DWARF register for use in OCaml frametable: "
      + Twine(DwarfRegNum));
  }
}

bool OcamlGCMetadataPrinter::emitStackMaps(Module &M, StackMaps &SM, AsmPrinter &AP) {
  MCStreamer &OS = *AP.OutStreamer;
  unsigned PtrSize = M.getDataLayout().getPointerSize(); // Can only be 8 for now
  
  OS.switchSection(AP.getObjFileLowering().getDataSection());
  
  emitCamlGlobal(M, OS, "frametable");

  // Number of records
  OS.emitInt64(SM.getCSInfos().size());

  for (const auto &CSI : SM.getCSInfos()) {
    // From runtime/frame_descriptors.h:
    // https://github.com/oxcaml/oxcaml/blob/main/runtime/caml/frame_descriptors.h#L63
    //
    // typedef struct {
    //   int32_t retaddr_rel; /* offset of return address from &retaddr_rel */
    //   uint16_t frame_data; /* frame size and various flags */
    //   uint16_t num_live;
    //   uint16_t live_ofs[num_live];
    // } frame_descr;

    // retaddr_rel
    MCSymbol *Here = OS.getContext().createTempSymbol();
    OS.emitLabel(Here);
    const MCExpr *RelativeAddr = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(CSI.CSLabel, OS.getContext()),
        MCSymbolRefExpr::create(Here, OS.getContext()),
        OS.getContext());
    OS.emitValue(RelativeAddr, 4);

    // frame_data
    uint64_t FrameSize = CSI.CSFunctionInfo.StaticStackSize;
    if (CSI.ID != StatepointDirectives::DefaultStatepointID)
      FrameSize += CSI.ID; // Stack offset from OxCaml
    FrameSize += PtrSize; // Return address

    if (FrameSize >= 1 << 16)
      report_fatal_error("Long frames not supported for OCaml GC: FrameSize = "
        + Twine(FrameSize));
    OS.emitInt16(FrameSize);

    // num_live
    uint64_t LiveCount = 0;
    for (const auto &Loc : CSI.Locations) {
      if (Loc.Type == StackMaps::Location::Register ||
          Loc.Type == StackMaps::Location::Direct ||
          Loc.Type == StackMaps::Location::Indirect) {
        LiveCount++;
      }
    }
    LiveCount += CSI.LiveOuts.size();

    if (LiveCount >= 1 << 16)
      report_fatal_error("Long frames not supported for OCaml GC: LiveCount = "
        + Twine(LiveCount));
    OS.emitInt16(LiveCount);

    // live_ofs
    for (const auto &Loc : CSI.Locations) {
      if (Loc.Type == StackMaps::Location::Register) {
        // Register indices are tagged (2n+1) and follow the OCaml register
        // map (see `mapLLVMDwarfRegToOCamlIndex`)
        unsigned DwarfRegNum = Loc.Reg;
        unsigned OCamlIndex = mapLLVMDwarfRegToOCamlIndex(DwarfRegNum);
        uint16_t EncodedReg = (OCamlIndex << 1) + 1;
        OS.emitInt16(EncodedReg);
      } else if (Loc.Type == StackMaps::Location::Direct ||
                 Loc.Type == StackMaps::Location::Indirect) {
        // For stack locations (Direct/Indirect): emit offset directly
        int64_t Offset = Loc.Offset;

        // BP-relative addressing -> SP
        if (Offset < 0) {
          int64_t TempFrameSize =
            FrameSize - PtrSize /* return address */ - PtrSize /* pushed BP */;
          Offset += TempFrameSize;
        }
        
        if (Offset < -(1 << 15) || Offset >= (1 << 15)) {
          report_fatal_error("Stack offset too large for OxCaml frametable: "
            + Twine(Offset));
        }
        OS.emitInt16(static_cast<uint16_t>(Offset));
      } else {
        // CR yusumez: Do we need anything else?
      }
    }

    for (const auto &LO : CSI.LiveOuts) {
      unsigned OCamlIndex = mapLLVMDwarfRegToOCamlIndex(LO.DwarfRegNum);
      uint16_t EncodedReg = (OCamlIndex << 1) + 1;
      OS.emitInt16(EncodedReg);
    }

    OS.emitValueToAlignment(Align(PtrSize));
  }

  OS.addBlankLine();
  return true;
}
