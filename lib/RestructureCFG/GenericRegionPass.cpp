//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "revng-c/RestructureCFG/GenericRegionInfo.h"
#include "revng-c/RestructureCFG/GenericRegionPass.h"

char GenericRegionPass::ID = 0;

static constexpr const char *Flag = "generic-region-info";
using Reg = llvm::RegisterPass<GenericRegionPass>;
static Reg X(Flag, "Perform the generic region identification analysis");

bool GenericRegionPass::runOnFunction(llvm::Function &F) {

  // Run the `GenericRegionInfo`
  GRI.clear();
  GRI.compute(&F);

  // The goal of `RegionIdentificationPass` is to just perform an analysis
  // which does not perform changes to the IR
  return false;
}

void GenericRegionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {

  // This is a read only analysis, that does not touch the IR
  AU.setPreservesAll();
}

const GenericRegionInfo<llvm::Function *> &GenericRegionPass::getResult() {
  return GRI;
}