//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// local includes
#include "revng-c/Liveness/LivenessAnalysis.h"

using namespace llvm;

namespace LivenessAnalysis {

llvm::Optional<LiveSet>
Analysis::handleEdge(const LiveSet &Original,
                             llvm::BasicBlock *Source,
                             llvm::BasicBlock *Destination) const {
  llvm::Optional<LiveSet> Result;

  auto SrcIt = PHIEdges.find(Source);
  if (SrcIt == PHIEdges.end())
    return Result;

  auto UseIt = SrcIt->second.find(Destination);
  revng_assert(UseIt != SrcIt->second.end());
  const UseSet &Pred = UseIt->second;
  for (Use *P : Pred) {
    auto *ThePHI = cast<PHINode>(P->getUser());
    auto *LiveI = dyn_cast<Instruction>(P->get());
    for (Value *V : ThePHI->incoming_values()) {
      if (auto *VInstr = dyn_cast<Instruction>(V)) {
        if (VInstr != LiveI) {
          // lazily copy the Original only if necessary
          if (not Result.hasValue())
            Result = Original.copy();
          Result->erase(VInstr);
        }
      }
    }
  }

  return Result;
}

Analysis::InterruptType Analysis::transfer(llvm::BasicBlock *BB) {
  LiveSet Result = State[BB].copy();
  auto RIt = BB->rbegin();
  auto REnd= BB->rend();
  for (; RIt != REnd; ++RIt) {
    Instruction &I = *RIt;

    if (auto *PHI = dyn_cast<PHINode>(&I))
      for (Use &U : PHI->incoming_values())
        PHIEdges[BB][PHI->getIncomingBlock(U)].insert(&U);

    for (Use &U : I.operands())
      if (auto *OpInst = dyn_cast<Instruction>(U))
        Result.insert(OpInst);

    Result.erase(&I);
  }
  return InterruptType::createInterrupt(std::move(Result));
}

} // end namespace LivenessAnalysis