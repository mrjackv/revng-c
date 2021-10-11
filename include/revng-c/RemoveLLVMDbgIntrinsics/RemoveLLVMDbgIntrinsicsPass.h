#pragma once

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include "llvm/Pass.h"

class RemoveLLVMDbgIntrinsicsPass : public llvm::FunctionPass {
public:
  static char ID;

public:
  RemoveLLVMDbgIntrinsicsPass() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
};