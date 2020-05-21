//
// Copyright (c) rev.ng Srls. See LICENSE.md for details.
//

#include <memory>
#include <type_traits>

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/Support/Debug.h"

#include "DLAHelpers.h"
#include "DLAStep.h"
#include "DLATypeSystem.h"

using namespace llvm;

static Logger<> Log("dla-compute-upper-member-access");

namespace dla {

bool ComputeUpperMemberAccesses::runOnTypeSystem(LayoutTypeSystem &TS) {
  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG() and TS.verifyInheritanceTree());
  bool Changed = false;

  using LTSN = LayoutTypeSystemNode;

  std::set<const LTSN *> Visited;
  for (LTSN *Root : llvm::nodes(&TS)) {
    revng_assert(Root != nullptr);
    // Leaves need to have ValidLayouts, otherwise they should have been trimmed
    // by PruneLayoutNodesWithoutLayout
    revng_assert(not isLeaf(Root) or hasValidLayout(Root));
    if (not isRoot(Root))
      continue;

    revng_assert(isInheritanceRoot(Root));

    for (LTSN *N : post_order_ext(Root, Visited)) {
      revng_assert(not isLeaf(N) or hasValidLayout(N));
      revng_assert(not N->L.Size);
      auto FinalSize = N->L.Size;

      // Accumulate sizes of accesses associated to N
      for (const Use *U : N->L.Accesses) {
        FinalSize = std::max(FinalSize, getLoadStoreSizeFromPtrOpUse(TS, U));
        revng_assert(FinalSize);
      }

      // Look at all the instance-of edges and inheritance edges all together.
      bool HasBaseClass = false;
      for (auto &[Child, EdgeTag] : children_edges<const LTSN *>(N)) {

        auto ChildSize = Child->L.Size;

        if (ChildSize <= 0LL)
          continue;

        switch (EdgeTag->getKind()) {

        case TypeLinkTag::LK_Inheritance: {
          // Treated as instance at offset 0, but can only have one.
          // Should only have one parent in inheritance hierarchy.
          revng_assert(not HasBaseClass);
          HasBaseClass = true;
          FinalSize = std::max(FinalSize, ChildSize);
        } break;

        case TypeLinkTag::LK_Instance: {
          const OffsetExpression &OE = EdgeTag->getOffsetExpr();
          revng_assert(OE.Strides.size() == OE.TripCounts.size());

          // Ignore stuff at negative offsets.
          if (OE.Offset < 0LL)
            continue;

          // If we have an array, we have to compute its size, taking into
          // account the strides and the trip counts.
          for (const auto &[TripCount, Stride] :
               llvm::reverse(llvm::zip(OE.TripCounts, OE.Strides))) {

            // Strides should be positive. If they are not, we don't know
            // anything about how the children is layed out, so we assume the
            // children doesn't even exist.
            if (Stride <= 0LL) {
              ChildSize = 0ULL;
              break;
            }

            auto StrideSize = static_cast<uint64_t>(Stride);

            // If we have a TripCount, we expect it to be strictly positive.
            revng_assert(not TripCount.has_value() or TripCount.value() > 0LL);

            // Arrays with unknown numbers of elements are considered as if
            // they had a single element
            auto NumElems = TripCount.has_value() ? TripCount.value() : 1;
            revng_assert(NumElems);

            // Here we are computing the larger size that is known to be
            // accessed. So if we have an array, we consider it to be one
            // element shorter than expected, and we add ChildSize only once
            // at the end.
            // This is equivalent to:
            // ChildSize = (NumElems * StrideSize) - (StrideSize - ChildSize);
            ChildSize = ((NumElems - 1) * StrideSize) + ChildSize;
          }

          if (not ChildSize)
            break;

          int64_t ChildOffset = std::max<int64_t>(OE.Offset, 0LL);
          uint64_t ChildUpperOffset = ChildOffset + ChildSize;
          FinalSize = std::max(FinalSize, ChildUpperOffset);
        } break;

        default:
          revng_unreachable("unexpected edge");
        }
      }
      N->L.Size = FinalSize;
      Changed = true;
    }
  }

  if (Log.isEnabled())
    TS.dumpDotOnFile("after-compute-upper-member-access.dot");

  return Changed;
}

} // end namespace dla